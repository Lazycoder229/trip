#include "tvm.h"
#include <curl/curl.h>
#include "../cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

extern Value cjsonToTrip(cJSON* node);
extern cJSON* tripToCJSON(Value val);

// ── HTTP response buffer ──────────────────────────────────────────────────
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} HttpBuf;

static size_t httpWriteCb(char* ptr, size_t sz, size_t nmemb, void* ud) {
    (void)sz;
    HttpBuf* b = (HttpBuf*)ud;
    size_t incoming = nmemb;
    size_t needed   = b->len + incoming + 1;
    if (needed > b->cap) {
        size_t newCap = b->cap == 0 ? 4096 : b->cap * 2;
        while (newCap < needed) newCap *= 2;
        char* tmp = realloc(b->data, newCap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = newCap;
    }
    memcpy(b->data + b->len, ptr, incoming);
    b->len += incoming;
    b->data[b->len] = '\0';
    return incoming;
}

static size_t httpHeaderCb(char* ptr, size_t sz, size_t nmemb, void* ud) {
    (void)sz;
    HttpBuf* b = (HttpBuf*)ud;
    size_t incoming = nmemb;
    size_t needed   = b->len + incoming + 1;
    if (needed > b->cap) {
        size_t newCap = b->cap == 0 ? 4096 : b->cap * 2;
        while (newCap < needed) newCap *= 2;
        char* tmp = realloc(b->data, newCap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = newCap;
    }
    memcpy(b->data + b->len, ptr, incoming);
    b->len += incoming;
    b->data[b->len] = '\0';
    return incoming;
}

static ObjDict* parseResponseHeaders(const char* raw, size_t rawLen) {
    ObjDict* dict = newDict();
    const char* p   = raw;
    const char* end = raw + rawLen;
    while (p < end) {
        const char* nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t lineLen = (size_t)(nl - p);
        if (lineLen > 0 && p[lineLen - 1] == '\r') lineLen--;
        if (lineLen == 0 || (lineLen >= 4 && strncmp(p, "HTTP", 4) == 0)) {
            p = nl + 1; continue;
        }
        const char* colon = p;
        while (colon < p + lineLen && *colon != ':') colon++;
        if (colon >= p + lineLen) { p = nl + 1; continue; }
        size_t nameLen = (size_t)(colon - p);
        const char* valStart = colon + 1;
        while (valStart < p + lineLen && *valStart == ' ') valStart++;
        size_t valLen = (size_t)((p + lineLen) - valStart);
        ObjString* key = copyString(p,        (int)nameLen);
        ObjString* val = copyString(valStart, (int)valLen);
        dictSet(dict, key, (Value){VAL_OBJ, {.obj = (Obj*)val}});
        p = nl + 1;
    }
    return dict;
}

// ── curl_multi global state ───────────────────────────────────────────────
// One CURLM handle shared across all fibers. Initialized once on first use.

static CURLM* g_curlMulti = NULL;

// Per-request context: tracks which fiber is waiting and accumulates the
// response so we can push the result dict when the request finishes.
typedef struct HttpPendingRequest {
    CURL*    easy;
    Fiber*   fiber;         // the fiber blocked on this request
    HttpBuf  resp;
    HttpBuf  respHdrs;
    bool     failed;
    char     errMsg[256];
    long     httpStatus;
    struct curl_slist* curlHeaders;
    char*    bodyStr;       // owned copy of POST body (or NULL)
    bool     ownedBodyStr;
    struct HttpPendingRequest* next;
} HttpPendingRequest;

// Linked list of all in-flight requests.
static HttpPendingRequest* g_pendingHead = NULL;

void httpMultiInit(void) {
    if (!g_curlMulti) {
        curl_global_init(CURL_GLOBAL_ALL);
        g_curlMulti = curl_multi_init();
    }
}

// Called from pollBlockedFibers() in scheduler.c — drives curl_multi and
// wakes up any fiber whose request has completed.
void httpMultiPoll(void) {
    if (!g_curlMulti || !g_pendingHead) return;

    // Drive curl without blocking (timeout=0).
    int running = 0;
    curl_multi_perform(g_curlMulti, &running);

    // Check for completed transfers.
    CURLMsg* msg;
    int msgsLeft = 0;
    while ((msg = curl_multi_info_read(g_curlMulti, &msgsLeft)) != NULL) {
        if (msg->msg != CURLMSG_DONE) continue;

        CURL* easy = msg->easy_handle;
        CURLcode res = msg->data.result;

        // Find the matching pending request.
        HttpPendingRequest* prev = NULL;
        HttpPendingRequest* req  = g_pendingHead;
        while (req && req->easy != easy) {
            prev = req;
            req  = req->next;
        }
        if (!req) continue; // shouldn't happen

        // Unlink from list.
        if (prev) prev->next = req->next;
        else       g_pendingHead = req->next;

        curl_multi_remove_handle(g_curlMulti, easy);

        if (res != CURLE_OK) {
            req->failed = true;
            snprintf(req->errMsg, sizeof(req->errMsg),
                     "http*(): request failed: %s", curl_easy_strerror(res));
        } else {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &req->httpStatus);
        }
        curl_easy_cleanup(easy);
        req->easy = NULL;

        if (req->curlHeaders) {
            curl_slist_free_all(req->curlHeaders);
            req->curlHeaders = NULL;
        }
        if (req->ownedBodyStr) {
            free(req->bodyStr);
            req->bodyStr = NULL;
        }

        // The fiber was parked in the blocked queue with waitFd =
        // TRIP_INVALID_SOCKET and a special marker. We re-queue it into
        // the ready queue so the scheduler resumes it. We store req in
        // the fiber's stack so the resume path can find it — use a raw
        // pointer cast into a number Value as a cheap handle. The fiber's
        // ip was rewound before blocking so it will re-enter
        // callBuiltinHTTP; we stash the completed req pointer so it can
        // skip the blocking path and build the result dict directly.
        //
        // Simpler approach: store req pointer on the Fiber via a dedicated
        // field. We add `void* httpPendingReq` to Fiber in vm.h.
        req->fiber->httpPendingReq = req;
        dequeueBlocked(req->fiber); // must unlink before re-queuing (see dequeueBlocked)
        req->fiber->state = FIBER_READY;
        enqueueReady(req->fiber);
    }
}

// Whether there's at least one HTTP request still in flight. Used by the
// scheduler to avoid blocking forever in poll() while curl still has work
// to finish — those fibers aren't tied to a real, poll()-able fd.
bool httpHasPending(void) {
    return g_pendingHead != NULL;
}

void httpMultiFree(void) {
    if (g_curlMulti) {
        // Cancel all pending requests.
        HttpPendingRequest* req = g_pendingHead;
        while (req) {
            HttpPendingRequest* next = req->next;
            if (req->easy) {
                curl_multi_remove_handle(g_curlMulti, req->easy);
                curl_easy_cleanup(req->easy);
            }
            if (req->curlHeaders) curl_slist_free_all(req->curlHeaders);
            if (req->ownedBodyStr) free(req->bodyStr);
            free(req->resp.data);
            free(req->respHdrs.data);
            free(req);
            req = next;
        }
        g_pendingHead = NULL;
        curl_multi_cleanup(g_curlMulti);
        g_curlMulti = NULL;
    }
}

// ── Main HTTP builtin ─────────────────────────────────────────────────────
InterpretResult callBuiltinHTTP(uint8_t id, uint8_t argc) {

    // ── Resume path: fiber is returning from a yield ──────────────────────
    // If this fiber has a completed request attached, build the result dict
    // and return without re-doing any curl setup.
    if (vm.current->httpPendingReq != NULL) {
        HttpPendingRequest* req = (HttpPendingRequest*)vm.current->httpPendingReq;
        vm.current->httpPendingReq = NULL;

        if (req->failed) {
            char msg[256];
            strncpy(msg, req->errMsg, sizeof(msg) - 1);
            msg[sizeof(msg) - 1] = '\0';
            free(req->resp.data);
            free(req->respHdrs.data);
            free(req);
            return raiseError("%s", msg);
        }

        ObjDict* result = newDict();
        push((Value){VAL_OBJ, {.obj = (Obj*)result}}); // GC root

        dictSet(result, copyString("status", 6),
                NUMBER_VAL((double)req->httpStatus));

        const char* bodyData = req->resp.data ? req->resp.data : "";
        ObjString* bodyOStr = copyString(bodyData, (int)strlen(bodyData));
        dictSet(result, copyString("body", 4),
                (Value){VAL_OBJ, {.obj = (Obj*)bodyOStr}});
        free(req->resp.data);

        ObjDict* hdrDict = parseResponseHeaders(
            req->respHdrs.data ? req->respHdrs.data : "", req->respHdrs.len);
        free(req->respHdrs.data);
        dictSet(result, copyString("headers", 7),
                (Value){VAL_OBJ, {.obj = (Obj*)hdrDict}});

        free(req);
        return INTERPRET_OK;
    }

    // ── First-call path: parse args and start the request ─────────────────
    bool hasBody = (id == BUILTIN_HTTP_POST || id == BUILTIN_HTTP_PUT || id == BUILTIN_HTTP_PATCH);
    int maxArgs = hasBody ? 4 : 3;
    if (argc < 1 || argc > maxArgs) {
        if (hasBody) return raiseError("httpPost/httpPut/httpPatch: too many arguments (max %d)", maxArgs);
        else         return raiseError("httpGet/httpDelete: too many arguments (max %d)", maxArgs);
    }

    #define HAS_OPTS_KEY(d, k, klen) ({ Value _ov; dictGet((d), copyString((k),(klen)), &_ov); })
    #define IS_OPTS_DICT(v) (IS_DICT(v) && ( \
        HAS_OPTS_KEY(AS_DICT(v), "timeout", 7) || \
        HAS_OPTS_KEY(AS_DICT(v), "follow",  6) || \
        HAS_OPTS_KEY(AS_DICT(v), "verify",  6)   \
    ))

    Value optsVal = NIL_VAL, bodyVal = NIL_VAL, headersVal = NIL_VAL, urlVal = NIL_VAL;
    if (hasBody) {
        if (argc == 4) { optsVal = pop(); bodyVal = pop(); headersVal = pop(); urlVal = pop(); }
        else if (argc == 3) {
            Value last = pop();
            if (IS_OPTS_DICT(last)) { optsVal = last; bodyVal = pop(); headersVal = pop(); }
            else { bodyVal = last; headersVal = pop(); }
            urlVal = pop();
        } else if (argc == 2) {
            Value last = pop();
            if (IS_OPTS_DICT(last)) optsVal = last;
            else bodyVal = last;
            urlVal = pop();
        } else { urlVal = pop(); }
    } else {
        if (argc == 3) { optsVal = pop(); headersVal = pop(); urlVal = pop(); }
        else if (argc == 2) {
            Value last = pop();
            if (IS_OPTS_DICT(last)) optsVal = last;
            else headersVal = last;
            urlVal = pop();
        } else { urlVal = pop(); }
    }
    #undef IS_OPTS_DICT
    #undef HAS_OPTS_KEY

    if (!IS_STRING(urlVal)) return raiseError("http*(): first argument must be a URL string");
    const char* url = AS_STRING(urlVal)->chars;

    long timeoutSecs = 30L;
    long followRedir = 1L;
    long verifySsl   = 1L;
    if (IS_DICT(optsVal)) {
        ObjDict* od = AS_DICT(optsVal);
        Value tmp;
        if (dictGet(od, copyString("timeout", 7), &tmp) && IS_NUMBER(tmp)) timeoutSecs = (long)AS_NUMBER(tmp);
        if (dictGet(od, copyString("follow",  6), &tmp) && IS_BOOL(tmp))   followRedir = AS_BOOL(tmp) ? 1L : 0L;
        if (dictGet(od, copyString("verify",  6), &tmp) && IS_BOOL(tmp))   verifySsl   = AS_BOOL(tmp) ? 1L : 0L;
    }

    char* bodyStr = NULL;
    bool  ownedBodyStr = false;
    if (!IS_NIL(bodyVal)) {
        if (IS_STRING(bodyVal)) {
            // Must dup because the ObjString may move under GC.
            int len = AS_STRING(bodyVal)->length;
            bodyStr = malloc((size_t)len + 1);
            memcpy(bodyStr, AS_STRING(bodyVal)->chars, (size_t)len + 1);
            ownedBodyStr = true;
        } else {
            cJSON* node = tripToCJSON(bodyVal);
            if (!node) return raiseError("http*(): could not serialize body to JSON");
            bodyStr = cJSON_PrintUnformatted(node);
            cJSON_Delete(node);
            ownedBodyStr = true;
        }
    }

    httpMultiInit();

    CURL* easy = curl_easy_init();
    if (!easy) {
        if (ownedBodyStr) free(bodyStr);
        return raiseError("http*(): failed to init curl");
    }

    // Allocate request context — will be freed on the resume path.
    HttpPendingRequest* req = (HttpPendingRequest*)calloc(1, sizeof(HttpPendingRequest));
    if (!req) {
        curl_easy_cleanup(easy);
        if (ownedBodyStr) free(bodyStr);
        return raiseError("http*(): out of memory");
    }
    req->easy         = easy;
    req->fiber        = vm.current;
    req->bodyStr      = bodyStr;
    req->ownedBodyStr = ownedBodyStr;

    curl_easy_setopt(easy, CURLOPT_URL,            url);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,  httpWriteCb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,      &req->resp);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, httpHeaderCb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA,     &req->respHdrs);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, followRedir);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT,        timeoutSecs);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, verifySsl);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, verifySsl ? 2L : 0L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,      "TripLang/1.0");
    // Non-blocking: disable signals so curl doesn't mess with the process.
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL,       1L);

    if (id == BUILTIN_HTTP_POST) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS,    bodyStr ? bodyStr : "");
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, bodyStr ? (long)strlen(bodyStr) : 0L);
    } else if (id == BUILTIN_HTTP_PUT) {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
        if (bodyStr) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS,    bodyStr);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(bodyStr));
        }
    } else if (id == BUILTIN_HTTP_PATCH) {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (bodyStr) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS,    bodyStr);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(bodyStr));
        }
    } else if (id == BUILTIN_HTTP_DELETE) {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    if (!IS_NIL(headersVal)) {
        if (!IS_DICT(headersVal)) {
            curl_easy_cleanup(easy);
            if (ownedBodyStr) free(bodyStr);
            free(req);
            return raiseError("http*(): headers must be a dict");
        }
        ObjDict* dict = AS_DICT(headersVal);
        for (int i = 0; i < dict->capacity; i++) {
            ObjString* key = dict->entries[i].key;
            if (key == NULL || key == DICT_TOMBSTONE) continue;
            Value val = dict->entries[i].value;
            if (!IS_STRING(val)) continue;
            int hlen = key->length + 2 + AS_STRING(val)->length + 1;
            char* hdr = malloc((size_t)hlen);
            if (hdr) {
                snprintf(hdr, (size_t)hlen, "%s: %s", key->chars, AS_STRING(val)->chars);
                req->curlHeaders = curl_slist_append(req->curlHeaders, hdr);
                free(hdr);
            }
        }
        if (req->curlHeaders)
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req->curlHeaders);
    }

    // Add to multi handle and link into pending list.
    curl_multi_add_handle(g_curlMulti, easy);
    req->next    = g_pendingHead;
    g_pendingHead = req;

    // Park this fiber — ip rewound so it re-enters callBuiltinHTTP on resume.
    // We use TRIP_INVALID_SOCKET as waitFd to signal "not a TCP wait" to
    // pollBlockedFibers; httpMultiPoll() will wake us up instead.
    vm.current->state         = FIBER_BLOCKED_IO;
    vm.current->waitFd        = TRIP_INVALID_SOCKET;
    vm.current->waitForWrite  = false;
    vm.current->waitDeadlineMs = -1;
    vm.current->timedOut      = false;
    // Rewind ip so that when this fiber resumes it re-calls callBuiltinHTTP,
    // which will take the resume path (httpPendingReq != NULL) above.
    vm.current->frames[vm.current->frameCount - 1].ip -= 3; // OP_CALL_BUILTIN + id + argc
    enqueueBlocked(vm.current);

    Fiber* next = nextFiberToRun();
    if (next == NULL) {
        // No other fiber to run — drive curl ourselves until this request done.
        // This handles the single-fiber case (no spawn).
        while (g_pendingHead != NULL) {
            httpMultiPoll();
            // Also poll TCP blocked fibers so we don't starve them.
            pollBlockedFibers();
            // Small sleep to avoid busy-spinning.
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, NULL);
#endif
        }
        // Our fiber should now be in the ready queue.
        next = dequeueReady();
        if (!next) return raiseError("http*(): internal scheduler error");
    }

    next->state  = FIBER_RUNNING;
    vm.current   = next;
    return INTERPRET_YIELD;
}
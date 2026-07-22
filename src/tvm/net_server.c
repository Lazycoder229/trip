#include "tvm.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

InterpretResult callBuiltinServer(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_HTTP_PARSE: {
            if (argc != 1) return raiseError("httpParse() expects 1 argument (string)");
            Value rawVal = pop();
            if (!IS_STRING(rawVal)) return raiseError("httpParse() argument must be a string");
            const char* raw = AS_STRING(rawVal)->chars;
            int rawLen      = AS_STRING(rawVal)->length;

            const char* p   = raw;
            const char* end = raw + rawLen;

#define HTTP_SKIP_TO(delim) while (p < end && *p != (delim)) p++
#define HTTP_SKIP_PAST(delim) do { HTTP_SKIP_TO(delim); if (p < end) p++; } while(0)

            const char* methodStart = p;
            HTTP_SKIP_TO(' ');
            ObjString* method = copyString(methodStart, (int)(p - methodStart));
            push((Value){VAL_OBJ, {.obj = (Obj*)method}});
            if (p < end) p++;

            const char* fullPathStart = p;
            HTTP_SKIP_TO(' ');
            int fullPathLen = (int)(p - fullPathStart);
            if (p < end) p++;

            const char* verStart = p;
            while (p < end && *p != '\r' && *p != '\n') p++;
            ObjString* version = copyString(verStart, (int)(p - verStart));
            push((Value){VAL_OBJ, {.obj = (Obj*)version}});

            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;

            const char* qmark = memchr(fullPathStart, '?', (size_t)fullPathLen);
            ObjString* path;
            ObjDict*   queryDict = newDict();
            push((Value){VAL_OBJ, {.obj = (Obj*)queryDict}});

            if (qmark) {
                path = copyString(fullPathStart, (int)(qmark - fullPathStart));
                const char* qp  = qmark + 1;
                const char* qend = fullPathStart + fullPathLen;
                while (qp < qend) {
                    const char* kStart = qp;
                    while (qp < qend && *qp != '=' && *qp != '&') qp++;
                    int kLen = (int)(qp - kStart);
                    ObjString* qkey = copyString(kStart, kLen);
                    push((Value){VAL_OBJ, {.obj = (Obj*)qkey}});
                    const char* vStart = qp;
                    Value qval;
                    if (qp < qend && *qp == '=') {
                        qp++;
                        vStart = qp;
                        while (qp < qend && *qp != '&') qp++;
                        qval = (Value){VAL_OBJ, {.obj = (Obj*)copyString(vStart, (int)(qp - vStart))}};
                    } else {
                        qval = (Value){VAL_OBJ, {.obj = (Obj*)copyString("", 0)}};
                    }
                    push(qval);
                    if (kLen > 0) dictSet(queryDict, qkey, qval);
                    pop(); pop();
                    if (qp < qend && *qp == '&') qp++;
                }
            } else {
                path = copyString(fullPathStart, fullPathLen);
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)path}});

            ObjDict* headers = newDict();
            push((Value){VAL_OBJ, {.obj = (Obj*)headers}});
            int contentLength = -1;
            bool reqKeepAlive = (strncmp(version->chars, "HTTP/1.1", 8) == 0);

            while (p < end) {
                if (*p == '\r' || *p == '\n') break;

                const char* nameStart = p;
                while (p < end && *p != ':' && *p != '\r' && *p != '\n') p++;
                int nameLen = (int)(p - nameStart);

                char* nameBuf = (char*)malloc((size_t)(nameLen + 1));
                if (!nameBuf) return raiseError("httpParse() out of memory");
                for (int i = 0; i < nameLen; i++)
                    nameBuf[i] = (char)tolower((unsigned char)nameStart[i]);
                nameBuf[nameLen] = '\0';

                if (p < end && *p == ':') p++;
                while (p < end && *p == ' ') p++;

                const char* valStart = p;
                while (p < end && *p != '\r' && *p != '\n') p++;
                int valLen = (int)(p - valStart);

                ObjString* hKey = copyString(nameBuf, nameLen);
                free(nameBuf);
                push((Value){VAL_OBJ, {.obj = (Obj*)hKey}});
                ObjString* hVal = copyString(valStart, valLen);
                push((Value){VAL_OBJ, {.obj = (Obj*)hVal}});
                dictSet(headers, hKey, (Value){VAL_OBJ, {.obj = (Obj*)hVal}});
                pop(); pop();

                if (strncmp(hKey->chars, "content-length", 14) == 0)
                    contentLength = atoi(hVal->chars);

                if (strncmp(hKey->chars, "connection", 10) == 0) {
                    char connLow[32] = {0};
                    int copyLen = valLen < 31 ? valLen : 31;
                    for (int i = 0; i < copyLen; i++)
                        connLow[i] = (char)tolower((unsigned char)hVal->chars[i]);
                    reqKeepAlive = (strncmp(connLow, "keep-alive", 10) == 0);
                }

                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;
            }

            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;

            ObjString* body;
            if (contentLength >= 0) {
                int avail = (int)(end - p);
                int take  = contentLength < avail ? contentLength : avail;
                body = copyString(p, take);
            } else {
                body = copyString(p, (int)(end - p));
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)body}});

            ObjDict* result = newDict();
#define HTTP_SET(k, v) do { \
    ObjString* _k = copyString((k), (int)strlen(k)); \
    dictSet(result, _k, (Value){VAL_OBJ, {.obj = (Obj*)(v)}}); \
} while(0)
            HTTP_SET("method",  method);
            HTTP_SET("path",    path);
            HTTP_SET("version", version);
            HTTP_SET("headers", headers);
            HTTP_SET("query",   queryDict);
            HTTP_SET("body",    body);
            { ObjString* _k = copyString("keepAlive", 9);
              dictSet(result, _k, BOOL_VAL(reqKeepAlive)); }
#undef HTTP_SET
#undef HTTP_SKIP_TO
#undef HTTP_SKIP_PAST

            pop(); pop(); pop(); pop(); pop(); pop();

            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }

        case BUILTIN_HTTP_RESPONSE: {
            if (argc < 3 || argc > 4)
                return raiseError("httpResponse() expects 3-4 arguments (status, headers, body [, keepAlive])");

            bool keepAlive = false;
            if (argc == 4) {
                Value kaVal = pop();
                if (!IS_BOOL(kaVal))
                    return raiseError("httpResponse() keepAlive must be a bool");
                keepAlive = AS_BOOL(kaVal);
            }

            Value bodyVal    = pop();
            Value headersVal = pop();
            Value statusVal  = pop();

            if (!IS_NUMBER(statusVal)) return raiseError("httpResponse() status must be a number");
            if (!IS_STRING(bodyVal))   return raiseError("httpResponse() body must be a string");
            if (!IS_NIL(headersVal) && !IS_DICT(headersVal))
                return raiseError("httpResponse() headers must be a dict or nil");

            int         status   = (int)AS_NUMBER(statusVal);
            ObjString*  bodyStr  = AS_STRING(bodyVal);
            ObjDict*    hdrDict  = IS_DICT(headersVal) ? AS_DICT(headersVal) : NULL;

            const char* reason;
            switch (status) {
                case 100: reason = "Continue"; break;
                case 200: reason = "OK"; break;
                case 201: reason = "Created"; break;
                case 204: reason = "No Content"; break;
                case 301: reason = "Moved Permanently"; break;
                case 302: reason = "Found"; break;
                case 304: reason = "Not Modified"; break;
                case 400: reason = "Bad Request"; break;
                case 401: reason = "Unauthorized"; break;
                case 403: reason = "Forbidden"; break;
                case 404: reason = "Not Found"; break;
                case 405: reason = "Method Not Allowed"; break;
                case 409: reason = "Conflict"; break;
                case 422: reason = "Unprocessable Entity"; break;
                case 429: reason = "Too Many Requests"; break;
                case 500: reason = "Internal Server Error"; break;
                case 501: reason = "Not Implemented"; break;
                case 503: reason = "Service Unavailable"; break;
                default:  reason = "Unknown"; break;
            }

            char dateBuf[64];
            time_t now = time(NULL);
            struct tm* gmt = gmtime(&now);
            strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", gmt);

            const char* contentType = "text/plain; charset=utf-8";
            if (hdrDict) {
                ObjString* ctKey = copyString("content-type", 12);
                Value ctVal;
                if (dictGet(hdrDict, ctKey, &ctVal) && IS_STRING(ctVal))
                    contentType = AS_STRING(ctVal)->chars;
            }

            int cap = 512 + bodyStr->length;
            char* buf = (char*)malloc((size_t)cap);
            if (!buf) return raiseError("httpResponse() out of memory");
            int len = 0;

#define RESP_APPEND(fmt, ...) do { \
    int _n = snprintf(buf + len, (size_t)(cap - len), fmt, ##__VA_ARGS__); \
    if (len + _n >= cap) { \
        cap = (len + _n + 1) * 2; \
        buf = (char*)realloc(buf, (size_t)cap); \
        if (!buf) return raiseError("httpResponse() out of memory"); \
        snprintf(buf + len, (size_t)(cap - len), fmt, ##__VA_ARGS__); \
    } \
    len += _n; \
} while(0)

            RESP_APPEND("HTTP/1.1 %d %s\r\n", status, reason);
            RESP_APPEND("Date: %s\r\n", dateBuf);
            RESP_APPEND("Content-Type: %s\r\n", contentType);
            RESP_APPEND("Content-Length: %d\r\n", bodyStr->length);
            RESP_APPEND("Connection: %s\r\n", keepAlive ? "keep-alive" : "close");

            if (hdrDict) {
                for (int i = 0; i < hdrDict->capacity; i++) {
                    DictEntry* e = &hdrDict->entries[i];
                    if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
                    if (strncasecmp(e->key->chars, "content-type", 12) == 0) continue;
                    if (!IS_STRING(e->value)) continue;
                    RESP_APPEND("%s: %s\r\n", e->key->chars, AS_STRING(e->value)->chars);
                }
            }

            RESP_APPEND("\r\n");

            if (len + bodyStr->length >= cap) {
                cap = len + bodyStr->length + 1;
                buf = (char*)realloc(buf, (size_t)cap);
                if (!buf) return raiseError("httpResponse() out of memory");
            }
            memcpy(buf + len, bodyStr->chars, (size_t)bodyStr->length);
            len += bodyStr->length;

#undef RESP_APPEND

            ObjString* resp = copyString(buf, len);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)resp}});
            break;
        }

        case BUILTIN_HTTP_CHUNKED_START: {
            if (argc != 3)
                return raiseError("httpChunkedStart() expects 3 arguments (socket, status, headers)");
            Value headersVal = pop();
            Value statusVal  = pop();
            Value sockVal    = pop();

            if (!IS_SOCKET(sockVal)) return raiseError("httpChunkedStart() first arg must be a socket");
            if (!IS_NUMBER(statusVal)) return raiseError("httpChunkedStart() status must be a number");
            if (!IS_NIL(headersVal) && !IS_DICT(headersVal))
                return raiseError("httpChunkedStart() headers must be a dict or nil");

            ObjSocket* sock    = AS_SOCKET(sockVal);
            int        status  = (int)AS_NUMBER(statusVal);
            ObjDict*   hdrDict = IS_DICT(headersVal) ? AS_DICT(headersVal) : NULL;
            if (sock->closed) return raiseError("httpChunkedStart() socket is closed");

            const char* reason;
            switch (status) {
                case 200: reason = "OK";                    break;
                case 201: reason = "Created";               break;
                case 204: reason = "No Content";            break;
                case 206: reason = "Partial Content";       break;
                case 301: reason = "Moved Permanently";     break;
                case 302: reason = "Found";                 break;
                case 400: reason = "Bad Request";           break;
                case 401: reason = "Unauthorized";          break;
                case 403: reason = "Forbidden";             break;
                case 404: reason = "Not Found";             break;
                case 500: reason = "Internal Server Error"; break;
                default:  reason = "Unknown";               break;
            }

            char dateBuf[64];
            time_t now2 = time(NULL);
            struct tm* gmt2 = gmtime(&now2);
            strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", gmt2);

            const char* contentType = "text/plain; charset=utf-8";
            if (hdrDict) {
                ObjString* ctKey2 = copyString("Content-Type", 12);
                Value ctVal2;
                if (dictGet(hdrDict, ctKey2, &ctVal2) && IS_STRING(ctVal2))
                    contentType = AS_STRING(ctVal2)->chars;
                if (contentType == NULL || strcmp(contentType, "text/plain; charset=utf-8") == 0) {
                    ObjString* ctKeyLc = copyString("content-type", 12);
                    if (dictGet(hdrDict, ctKeyLc, &ctVal2) && IS_STRING(ctVal2))
                        contentType = AS_STRING(ctVal2)->chars;
                }
            }

            int   hcap = 512;
            char* hbuf = (char*)malloc((size_t)hcap);
            if (!hbuf) return raiseError("httpChunkedStart() out of memory");
            int   hlen = 0;

#define HC_APPEND(fmt, ...) do { \
    int _n = snprintf(hbuf + hlen, (size_t)(hcap - hlen), fmt, ##__VA_ARGS__); \
    if (hlen + _n >= hcap) { \
        hcap = (hlen + _n + 1) * 2; \
        hbuf = (char*)realloc(hbuf, (size_t)hcap); \
        if (!hbuf) return raiseError("httpChunkedStart() out of memory"); \
        snprintf(hbuf + hlen, (size_t)(hcap - hlen), fmt, ##__VA_ARGS__); \
    } \
    hlen += _n; \
} while(0)

            HC_APPEND("HTTP/1.1 %d %s\r\n", status, reason);
            HC_APPEND("Date: %s\r\n", dateBuf);
            HC_APPEND("Content-Type: %s\r\n", contentType);
            HC_APPEND("Transfer-Encoding: chunked\r\n");
            HC_APPEND("Cache-Control: no-cache\r\n");

            if (hdrDict) {
                for (int i = 0; i < hdrDict->capacity; i++) {
                    DictEntry* e = &hdrDict->entries[i];
                    if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
                    if (!IS_STRING(e->value)) continue;
                    const char* k = e->key->chars;
                    if (strncasecmp(k, "content-type",      12) == 0) continue;
                    if (strncasecmp(k, "transfer-encoding", 17) == 0) continue;
                    if (strncasecmp(k, "cache-control",     13) == 0) continue;
                    HC_APPEND("%s: %s\r\n", k, AS_STRING(e->value)->chars);
                }
            }

            HC_APPEND("\r\n");

#undef HC_APPEND

            bool ok = ws_send_all(sock->handle, hbuf, (size_t)hlen);
            free(hbuf);
            if (!ok) return raiseError("httpChunkedStart() failed to send headers");

            push(NIL_VAL);
            break;
        }

        case BUILTIN_HTTP_CHUNK_WRITE: {
            if (argc != 2) return raiseError("httpChunkWrite() expects 2 arguments (socket, data)");
            Value dataVal = pop();
            Value sockVal = pop();

            if (!IS_SOCKET(sockVal)) return raiseError("httpChunkWrite() first arg must be a socket");
            if (!IS_STRING(dataVal)) return raiseError("httpChunkWrite() second arg must be a string");
            ObjSocket* sock = AS_SOCKET(sockVal);
            if (sock->closed) return raiseError("httpChunkWrite() socket is closed");

            ObjString* data = AS_STRING(dataVal);
            if (data->length == 0) { push(NIL_VAL); break; }

            char chunkHdr[24];
            int  chunkHdrLen = snprintf(chunkHdr, sizeof(chunkHdr), "%x\r\n", (unsigned)data->length);

            if (!ws_send_all(sock->handle, chunkHdr, (size_t)chunkHdrLen) ||
                !ws_send_all(sock->handle, data->chars, (size_t)data->length) ||
                !ws_send_all(sock->handle, "\r\n", 2)) {
                return raiseError("httpChunkWrite() send failed");
            }
            push(NIL_VAL);
            break;
        }

        case BUILTIN_HTTP_CHUNK_END: {
            if (argc != 1) return raiseError("httpChunkEnd() expects 1 argument (socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("httpChunkEnd() expects a socket");
            ObjSocket* sock = AS_SOCKET(sockVal);
            if (sock->closed) return raiseError("httpChunkEnd() socket is closed");

            if (!ws_send_all(sock->handle, "0\r\n\r\n", 5))
                return raiseError("httpChunkEnd() send failed");

            push(NIL_VAL);
            break;
        }

        case BUILTIN_SSE_WRITE: {
            if (argc < 2 || argc > 4)
                return raiseError("sseWrite() expects 2-4 arguments (socket, data [, event [, id]])");

            ObjString* sseId    = NULL;
            ObjString* sseEvent = NULL;
            if (argc >= 4) {
                Value idVal = pop();
                if (!IS_STRING(idVal)) return raiseError("sseWrite() id must be a string");
                sseId = AS_STRING(idVal);
            }
            if (argc >= 3) {
                Value evVal = pop();
                if (!IS_STRING(evVal)) return raiseError("sseWrite() event must be a string");
                sseEvent = AS_STRING(evVal);
            }
            Value dataVal = pop();
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("sseWrite() first arg must be a socket");
            if (!IS_STRING(dataVal)) return raiseError("sseWrite() data must be a string");
            ObjSocket* sock = AS_SOCKET(sockVal);
            if (sock->closed) return raiseError("sseWrite() socket is closed");

            ObjString* data = AS_STRING(dataVal);

            int   ecap = 256 + data->length;
            char* ebuf = (char*)malloc((size_t)ecap);
            if (!ebuf) return raiseError("sseWrite() out of memory");
            int   elen = 0;

#define SSE_APPEND(fmt, ...) do { \
    int _n = snprintf(ebuf + elen, (size_t)(ecap - elen), fmt, ##__VA_ARGS__); \
    if (elen + _n >= ecap) { \
        ecap = (elen + _n + 1) * 2; \
        ebuf = (char*)realloc(ebuf, (size_t)ecap); \
        if (!ebuf) return raiseError("sseWrite() out of memory"); \
        snprintf(ebuf + elen, (size_t)(ecap - elen), fmt, ##__VA_ARGS__); \
    } \
    elen += _n; \
} while(0)

            if (sseId)    SSE_APPEND("id: %s\n", sseId->chars);
            if (sseEvent) SSE_APPEND("event: %s\n", sseEvent->chars);

            const char* p   = data->chars;
            const char* end = data->chars + data->length;
            while (p < end) {
                const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
                int lineLen = nl ? (int)(nl - p) : (int)(end - p);
                SSE_APPEND("data: ");
                if (elen + lineLen + 1 >= ecap) {
                    ecap = (elen + lineLen + 2) * 2;
                    ebuf = (char*)realloc(ebuf, (size_t)ecap);
                    if (!ebuf) return raiseError("sseWrite() out of memory");
                }
                memcpy(ebuf + elen, p, (size_t)lineLen);
                elen += lineLen;
                ebuf[elen++] = '\n';
                p = nl ? nl + 1 : end;
            }

            SSE_APPEND("\n");

#undef SSE_APPEND

            char chunkHdr2[24];
            int  chunkHdrLen2 = snprintf(chunkHdr2, sizeof(chunkHdr2), "%x\r\n", (unsigned)elen);
            bool ok2 = ws_send_all(sock->handle, chunkHdr2, (size_t)chunkHdrLen2) &&
                       ws_send_all(sock->handle, ebuf,       (size_t)elen)          &&
                       ws_send_all(sock->handle, "\r\n",     2);
            free(ebuf);
            if (!ok2) return raiseError("sseWrite() send failed");

            push(NIL_VAL);
            break;
        }

        default:
            return raiseError("Unknown server builtin %d", id);
    }
    return INTERPRET_OK;
}

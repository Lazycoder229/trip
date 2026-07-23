#include "tvm.h"

// Set once by main.c via vmSetArgs(), before the script starts running.
void vmSetArgs(int argc, const char* const* argv, const char* scriptPath) {
    g_scriptArgc = argc;
    g_scriptArgv = argv;
    g_scriptPath = scriptPath;
}

const char* vmGetScriptPath(void) {
    return g_scriptPath;
}

// ── resolveFilePath ────────────────────────────────────────────────────────
char* resolveFilePath(const char* givenPath) {
    bool isAbsolute = (givenPath[0] == '/') || (givenPath[0] == '\\') ||
                      (isalpha((unsigned char)givenPath[0]) && givenPath[1] == ':');
    char* rawPath = NULL;
    if (isAbsolute || g_scriptPath == NULL) {
        rawPath = malloc(strlen(givenPath) + 1);
        if (!rawPath) { fprintf(stderr, "[trip] OOM in resolveFilePath\n"); exit(1); }
        strcpy(rawPath, givenPath);
    } else {
        const char* lastFwd  = strrchr(g_scriptPath, '/');
        const char* lastBack = strrchr(g_scriptPath, '\\');
        const char* lastSlash = (lastFwd > lastBack) ? lastFwd : lastBack;
        size_t baseLen = lastSlash ? (size_t)(lastSlash - g_scriptPath + 1) : 0;
        size_t givenLen = strlen(givenPath);
        rawPath = malloc(baseLen + givenLen + 1);
        if (!rawPath) { fprintf(stderr, "[trip] OOM in resolveFilePath\n"); exit(1); }
        memcpy(rawPath, g_scriptPath, baseLen);
        memcpy(rawPath + baseLen, givenPath, givenLen + 1);
    }
#if defined(_POSIX_VERSION)
    if (g_scriptPath != NULL) {
        char scriptDir[PATH_MAX];
        char tmp[PATH_MAX];
        size_t spLen = strlen(g_scriptPath);
        if (spLen >= PATH_MAX) { free(rawPath); return NULL; }
        memcpy(tmp, g_scriptPath, spLen + 1);
        char* lastSep = strrchr(tmp, '/');
        if (!lastSep) lastSep = strrchr(tmp, '\\');
        if (lastSep) *lastSep = '\0';
        else { tmp[0] = '.'; tmp[1] = '\0'; }
        if (!realpath(tmp, scriptDir)) return rawPath;
        char canonical[PATH_MAX];
        bool resolved = (realpath(rawPath, canonical) != NULL);
        if (!resolved) {
            char parentTmp[PATH_MAX];
            size_t rawLen = strlen(rawPath);
            if (rawLen >= PATH_MAX) { free(rawPath); return NULL; }
            memcpy(parentTmp, rawPath, rawLen + 1);
            char* lastRawSep = strrchr(parentTmp, '/');
            if (!lastRawSep) lastRawSep = strrchr(parentTmp, '\\');
            const char* bareFile = rawPath;
            if (lastRawSep) {
                bareFile = lastRawSep + 1;
                *lastRawSep = '\0';
                char parentCanon[PATH_MAX];
                if (!realpath(parentTmp, parentCanon)) return rawPath;
                size_t pc = strlen(parentCanon);
                size_t bf = strlen(bareFile);
                if (pc + 1 + bf + 1 > PATH_MAX) { free(rawPath); return NULL; }
                parentCanon[pc] = '/';
                memcpy(parentCanon + pc + 1, bareFile, bf + 1);
                memcpy(canonical, parentCanon, pc + 1 + bf + 1);
            } else {
                size_t sd = strlen(scriptDir);
                size_t bf = strlen(bareFile);
                if (sd + 1 + bf + 1 > PATH_MAX) { free(rawPath); return NULL; }
                memcpy(canonical, scriptDir, sd);
                canonical[sd] = '/';
                memcpy(canonical + sd + 1, bareFile, bf + 1);
            }
        }
        size_t sdLen = strlen(scriptDir);
        bool inSandbox = (strncmp(canonical, scriptDir, sdLen) == 0) &&
                         (canonical[sdLen] == '/' || canonical[sdLen] == '\0');
        if (!inSandbox) { free(rawPath); return NULL; }
        free(rawPath);
        rawPath = malloc(strlen(canonical) + 1);
        if (!rawPath) { fprintf(stderr, "[trip] OOM in resolveFilePath\n"); exit(1); }
        strcpy(rawPath, canonical);
    }
#endif
    return rawPath;
}

// ── print any Value ────────────────────────────────────────────────────────
void printValue(Value value) {
    if (IS_NIL(value)) {
        printf("nil");
    } else if (IS_BOOL(value)) {
        printf("%s", AS_BOOL(value) ? "true" : "false");
    } else if (IS_NUMBER(value)) {
        double n = AS_NUMBER(value);
        if (n == (long long)n) printf("%lld", (long long)n);
        else printf("%g", n);
    } else if (IS_CHAR(value)) {
        printf("%c", AS_CHAR(value));
    } else if (IS_OBJ(value)) {
        Obj* obj = AS_OBJ(value);
        switch (obj->type) {
            case OBJ_STRING:
                printf("%s", ((ObjString*)obj)->chars);
                break;
            case OBJ_LIST: {
                ObjList* list = (ObjList*)obj;
                printf("[");
                for (int i = 0; i < list->count; i++) {
                    if (i) printf(", ");
                    printValue(list->items[i]);
                }
                printf("]");
                break;
            }
            case OBJ_DICT: {
                ObjDict* dict = (ObjDict*)obj;
                printf("{");
                int printed = 0;
                for (int i = 0; i < dict->capacity; i++) {
                    DictEntry* e = &dict->entries[i];
                    if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
                    if (printed++) printf(", ");
                    printf("\"%s\": ", e->key->chars);
                    printValue(e->value);
                }
                printf("}");
                break;
            }
            case OBJ_FUNCTION: {
                ObjFunction* function = (ObjFunction*)obj;
                printf("<fn %s>", function->name ? function->name->chars : "?");
                break;
            }
            case OBJ_CLOSURE: {
                ObjFunction* function = ((ObjClosure*)obj)->function;
                printf("<fn %s>", function->name ? function->name->chars : "?");
                break;
            }
            case OBJ_UPVALUE:
                printf("<upvalue>");
                break;
            case OBJ_SOCKET: {
                ObjSocket* sock = (ObjSocket*)obj;
                printf("<socket %s>", sock->closed ? "closed" :
                       (sock->isListening ? "listening" : "connected"));
                break;
            }
            case OBJ_DB_CONN: {
                ObjDBConn* db = (ObjDBConn*)obj;
                printf("<mysql connection %s>", db->closed ? "closed" : "open");
                break;
            }
            case OBJ_CLASS: {
                printf("<class %s>", ((ObjClass*)obj)->name->chars);
                break;
            }
            case OBJ_INSTANCE: {
                printf("<%s instance>", ((ObjInstance*)obj)->klass->name->chars);
                break;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = (ObjBoundMethod*)obj;
                printf("<fn %s>", bound->method->function->name->chars);
                break;
            }
        }
    } else {
        printf("<value>");
    }
}

const char* typeName(Value v) {
    if (IS_NIL(v))    return "nil";
    if (IS_BOOL(v))   return "bool";
    if (IS_NUMBER(v)) return "number";
    if (IS_CHAR(v))   return "char";
    if (IS_STRING(v)) return "string";
    if (IS_LIST(v))   return "list";
    if (IS_DICT(v))   return "dict";
    if (IS_FUNCTION(v) || IS_CLOSURE(v)) return "function";
    if (IS_SOCKET(v)) return "socket";
    return "unknown";
}

// ── Stack Helpers ─────────────────────────────────────────────────────────
void growStack() {
    Fiber* fb = vm.current;
    int newCap = fb->stackCapacity * 2;
    Value* newStack = (Value*)reallocate(fb->stack,
                                         sizeof(Value) * (size_t)fb->stackCapacity,
                                         sizeof(Value) * (size_t)newCap);
    ptrdiff_t delta = newStack - fb->stack;
    if (delta != 0) {
        for (ObjUpvalue* uv = fb->openUpvalues; uv != NULL; uv = uv->next) {
            if (uv->location >= fb->stack && uv->location < fb->stack + fb->stackCapacity) {
                uv->location += delta;
            }
        }
    }
    fb->stack = newStack;
    fb->stackCapacity = newCap;
}

void growFrames() {
    Fiber* fb = vm.current;
    int newCap = fb->frameCapacity * 2;
    fb->frames = (CallFrame*)reallocate(fb->frames,
                                       sizeof(CallFrame) * (size_t)fb->frameCapacity,
                                       sizeof(CallFrame) * (size_t)newCap);
    fb->frameCapacity = newCap;
}

void resetStack() {
    vm.current->stackCount = 0;
}

void push(Value value) {
    Fiber* fb = vm.current;
    if (fb->stackCount >= fb->stackCapacity) growStack();
    fb = vm.current;
    fb->stack[fb->stackCount++] = value;
}

Value pop() {
    Fiber* fb = vm.current;
    return fb->stack[--fb->stackCount];
}

Value peek(int dist) {
    Fiber* fb = vm.current;
    return fb->stack[fb->stackCount - 1 - dist];
}

// ── Upvalues ───────────────────────────────────────────────────────────────
ObjUpvalue* captureUpvalue(Value* local) {
    Fiber* fb = vm.current;
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue = fb->openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }
    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }
    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;
    if (prevUpvalue == NULL) {
        fb->openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }
    return createdUpvalue;
}

void closeUpvalues(Value* last) {
    Fiber* fb = vm.current;
    while (fb->openUpvalues != NULL && fb->openUpvalues->location >= last) {
        ObjUpvalue* upvalue = fb->openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        fb->openUpvalues = upvalue->next;
    }
}

// ── Const Tracking ─────────────────────────────────────────────────────────
void registerVar(const char* key, bool isConst) {
    for (int i = 0; i < vm.metaCount; i++) {
        if (strcmp(vm.meta[i].key, key) == 0) { vm.meta[i].isConst = isConst; return; }
    }
    if (vm.metaCount >= META_MAX) {
        fprintf(stderr, "[trip] Warning: too many global variables (max %d) — "
                        "const/let tracking disabled for '%s'\n", META_MAX, key);
        return;
    }
    vm.meta[vm.metaCount].key = strdup(key);
    if (!vm.meta[vm.metaCount].key) {
        fprintf(stderr, "[trip] Out of memory in registerVar\n");
        exit(1);
    }
    vm.meta[vm.metaCount].isConst = isConst;
    vm.metaCount++;
}

bool isConst(const char* key) {
    for (int i = 0; i < vm.metaCount; i++)
        if (strcmp(vm.meta[i].key, key) == 0) return vm.meta[i].isConst;
    return false;
}

// ── Value Equality & Falsiness ──────────────────────────────────────────────
bool valuesEqual(Value a, Value b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL:    return true;
        case VAL_BOOL:   return AS_BOOL(a) == AS_BOOL(b);
        case VAL_NUMBER: return AS_NUMBER(a) == AS_NUMBER(b);
        case VAL_CHAR:   return AS_CHAR(a) == AS_CHAR(b);
        case VAL_OBJ: {
            if (OBJ_TYPE(a) != OBJ_TYPE(b)) return false;
            if (OBJ_TYPE(a) == OBJ_STRING) {
                ObjString* sa = AS_STRING(a); ObjString* sb = AS_STRING(b);
                return sa->length == sb->length && memcmp(sa->chars, sb->chars, sa->length) == 0;
            }
            return AS_OBJ(a) == AS_OBJ(b);
        }
    }
    return false;
}

bool isFalsy(Value v) {
    if (IS_NIL(v))    return true;
    if (IS_BOOL(v))   return !AS_BOOL(v);
    if (IS_NUMBER(v)) return AS_NUMBER(v) == 0;
    return false;
}

ObjString* concatStrings(ObjString* a, ObjString* b) {
    int len = a->length + b->length;
    char* buf = malloc(len + 1);
    if (!buf) { fprintf(stderr, "[trip] OOM in concatStrings\n"); exit(1); }
    memcpy(buf, a->chars, a->length);
    memcpy(buf + a->length, b->chars, b->length);
    buf[len] = '\0';
    ObjString* s = copyString(buf, len);
    free(buf);
    return s;
}

// ── Try/Catch/Throw Support ───────────────────────────────────────────────
static void unwindToHandler(Value errVal) {
    TryHandler h = vm.current->tryHandlers[--vm.current->tryHandlerCount];
    closeUpvalues(vm.current->stack + h.stackTopIndex); // NOTE: closeUpvalues now takes Value*
    vm.current->frameCount = h.frameCount;
    vm.current->stackCount = h.stackTopIndex;
    vm.current->frames[vm.current->frameCount - 1].ip = h.catchIp;
    push(errVal);
}

void printStackTrace() {
    Fiber* fiber = vm.current;
    for (int i = fiber->frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &fiber->frames[i];
        ObjFunction* fn  = frame->closure->function;
        int offset = (int)(frame->ip - fn->chunk.code) - 1;
        int line   = getLine(&fn->chunk, offset);
        if (line < 0) fprintf(stderr, "  [line ?] in ");
        else fprintf(stderr, "  [line %d] in ", line);
        if (fn->name == NULL || fn->name->length == 0) fprintf(stderr, "<script>\n");
        else fprintf(stderr, "%s\n", fn->name->chars);
    }
}

InterpretResult raiseError(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (vm.current->tryHandlerCount > 0) {
        ObjString* msg = copyString(buf, (int)strlen(buf));
        unwindToHandler((Value){VAL_OBJ, {.obj = (Obj*)msg}});
        return INTERPRET_HANDLED_ERROR;
    }
    int line = -1;
    if (vm.current->frameCount > 0) {
        CallFrame* frame = &vm.current->frames[vm.current->frameCount - 1];
        ObjFunction* fn  = frame->closure->function;
        int offset = (int)(frame->ip - fn->chunk.code) - 1;
        line = getLine(&fn->chunk, offset);
    }
    if (line >= 0) fprintf(stderr, "[line %d] RuntimeError: %s\n", line, buf);
    else fprintf(stderr, "RuntimeError: %s\n", buf);
    printStackTrace();
    return INTERPRET_RUNTIME_ERROR;
}

InterpretResult raiseValue(Value errVal) {
    if (vm.current->tryHandlerCount > 0) {
        unwindToHandler(errVal);
        return INTERPRET_HANDLED_ERROR;
    }
    int line = -1;
    if (vm.current->frameCount > 0) {
        CallFrame* frame = &vm.current->frames[vm.current->frameCount - 1];
        ObjFunction* fn  = frame->closure->function;
        int offset = (int)(frame->ip - fn->chunk.code) - 1;
        line = getLine(&fn->chunk, offset);
    }
    if (line >= 0) fprintf(stderr, "[line %d] RuntimeError: Uncaught exception: ", line);
    else fprintf(stderr, "RuntimeError: Uncaught exception: ");
    printValue(errVal);
    fprintf(stderr, "\n");
    printStackTrace();
    return INTERPRET_RUNTIME_ERROR;
}

bool ws_send_all(TripSocketHandle sock, const char* buf, size_t len) {
    size_t sent=0;
    while (sent<len) {
        long n=send(sock, buf+sent, (int)(len-sent), 0);
        if (n<=0) return false;
        sent+=(size_t)n;
    }
    return true;
}

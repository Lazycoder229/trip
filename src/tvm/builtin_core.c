#include "tvm.h"

// Math builtins can also be grouped here since they are short.
InterpretResult callBuiltinMath(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_SQRT: {
            if (argc != 1) return raiseError("sqrt() expects 1 argument");
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("sqrt() expects a number");
            double x = AS_NUMBER(v);
            if (x < 0) return raiseError("sqrt() of a negative number");
            push(NUMBER_VAL(sqrt(x)));
            break;
        }
        case BUILTIN_POW: {
            if (argc != 2) return raiseError("pow() expects 2 arguments (base, exponent)");
            Value expVal = pop();
            Value baseVal = pop();
            if (!IS_NUMBER(baseVal) || !IS_NUMBER(expVal)) return raiseError("pow() expects two numbers");
            push(NUMBER_VAL(pow(AS_NUMBER(baseVal), AS_NUMBER(expVal))));
            break;
        }
        case BUILTIN_ABS: {
            if (argc != 1) return raiseError("abs() expects 1 argument");
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("abs() expects a number");
            push(NUMBER_VAL(fabs(AS_NUMBER(v))));
            break;
        }
        case BUILTIN_MIN:
        case BUILTIN_MAX: {
            if (argc < 1) return raiseError(id == BUILTIN_MIN ? "min() expects at least 1 argument" : "max() expects at least 1 argument");
            double best = 0;
            for (int i = argc - 1; i >= 0; i--) {
                Value v = pop();
                if (!IS_NUMBER(v)) return raiseError(id == BUILTIN_MIN ? "min() expects numbers" : "max() expects numbers");
                double n = AS_NUMBER(v);
                if (i == argc - 1) best = n;
                else if (id == BUILTIN_MIN) { if (n < best) best = n; }
                else { if (n > best) best = n; }
            }
            push(NUMBER_VAL(best));
            break;
        }
        case BUILTIN_FLOOR: {
            if (argc != 1) return raiseError("floor() expects 1 argument");
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("floor() expects a number");
            push(NUMBER_VAL(floor(AS_NUMBER(v))));
            break;
        }
        case BUILTIN_CEIL: {
            if (argc != 1) return raiseError("ceil() expects 1 argument");
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("ceil() expects a number");
            push(NUMBER_VAL(ceil(AS_NUMBER(v))));
            break;
        }
        case BUILTIN_ROUND: {
            if (argc != 1 && argc != 2) return raiseError("round() expects 1 or 2 arguments (value, [decimals])");
            double decimals = 0;
            if (argc == 2) {
                Value decVal = pop();
                if (!IS_NUMBER(decVal)) return raiseError("round() decimals must be a number");
                decimals = AS_NUMBER(decVal);
            }
            Value v = pop();
            if (!IS_NUMBER(v)) return raiseError("round() expects a number");
            double factor = pow(10, decimals);
            push(NUMBER_VAL(round(AS_NUMBER(v) * factor) / factor));
            break;
        }
        case BUILTIN_RANDOM: {
            if (argc != 0) return raiseError("random() takes no arguments");
            push(NUMBER_VAL((double)rand() / ((double)RAND_MAX + 1.0)));
            break;
        }
        case BUILTIN_RANDOM_INT: {
            if (argc != 2) return raiseError("randomInt() expects 2 arguments (min, max)");
            Value maxVal = pop();
            Value minVal = pop();
            if (!IS_NUMBER(minVal) || !IS_NUMBER(maxVal)) return raiseError("randomInt() expects two numbers");
            long lo = (long)AS_NUMBER(minVal);
            long hi = (long)AS_NUMBER(maxVal);
            if (hi < lo) return raiseError("randomInt() max must be >= min");
            unsigned long span = (unsigned long)(hi - lo) + 1UL;
            unsigned long r = (unsigned long)rand();
            push(NUMBER_VAL((double)(lo + (long)(r % span))));
            break;
        }
        default:
            return raiseError("Unknown Math builtin %d", id);
    }
    return INTERPRET_OK;
}

InterpretResult callBuiltinCore(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_LEN: {
            Value v = pop();
            if (IS_STRING(v)) push(NUMBER_VAL(AS_STRING(v)->length));
            else if (IS_LIST(v)) push(NUMBER_VAL(AS_LIST(v)->count));
            else if (IS_DICT(v)) push(NUMBER_VAL(AS_DICT(v)->count));
            else return raiseError("len() expects string/list/dict");
            break;
        }
        case BUILTIN_TYPE: {
            Value v = pop();
            ObjString* s = copyString(typeName(v), strlen(typeName(v)));
            push((Value){VAL_OBJ, {.obj=(Obj*)s}});
            break;
        }
        case BUILTIN_INT: {
            Value v = pop();
            if (IS_NUMBER(v)) { push(NUMBER_VAL((double)(long long)AS_NUMBER(v))); break; }
            if (IS_CHAR(v))   { push(NUMBER_VAL((double)AS_CHAR(v))); break; }
            if (IS_BOOL(v))   { push(NUMBER_VAL(AS_BOOL(v) ? 1.0 : 0.0)); break; }
            if (IS_STRING(v)) {
                const char* s = AS_STRING(v)->chars;
                char* end;
                double result = strtod(s, &end);
                while (*end == ' ') end++;
                if (end == s || *end != '\0') return raiseError("int() cannot convert \"%s\" to number", s);
                push(NUMBER_VAL((double)(long long)result));
                break;
            }
            return raiseError("int() cannot convert %s", typeName(v));
        }
        case BUILTIN_FLOAT: {
            Value v = pop();
            if (IS_NUMBER(v)) { push(v); break; }
            if (IS_STRING(v)) {
                const char* s = AS_STRING(v)->chars;
                char* end;
                double result = strtod(s, &end);
                while (*end == ' ') end++;
                if (end == s || *end != '\0') return raiseError("float() cannot convert \"%s\" to number", s);
                push(NUMBER_VAL(result));
                break;
            }
            return raiseError("float() cannot convert %s", typeName(v));
        }
        case BUILTIN_STR: {
            Value v = pop();
            if (IS_STRING(v))  { push(v); break; }
            if (IS_NIL(v))     { push((Value){VAL_OBJ,{.obj=(Obj*)copyString("nil",3)}}); break; }
            if (IS_BOOL(v)) {
                const char* s = AS_BOOL(v) ? "true" : "false";
                push((Value){VAL_OBJ,{.obj=(Obj*)copyString(s,strlen(s))}});
                break;
            }
            if (IS_CHAR(v)) {
                char buf[2] = { AS_CHAR(v), '\0' };
                push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf,1)}});
                break;
            }
            if (IS_NUMBER(v)) {
                char buf[64];
                double n = AS_NUMBER(v);
                if (n == (long long)n) snprintf(buf, sizeof(buf), "%lld", (long long)n);
                else snprintf(buf, sizeof(buf), "%g", n);
                push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf,strlen(buf))}});
                break;
            }
            // Compound types: fallback to original logic for complex stringification.
            // For now, doing a simpler stringify if you just type `str([1,2,3])` in scripts.
            // Ideally we migrate the large `str()` logic here.
            {
                int bufCap = 256;
                int bufLen = 0;
                char* buf = malloc((size_t)bufCap);
                if (!buf) { push((Value){VAL_OBJ,{.obj=(Obj*)copyString("<?>",3)}}); break; }

                #define SBUF_APPEND(s, n) do { \
                    int _n = (int)(n); \
                    while (bufLen + _n + 1 > bufCap) { \
                        bufCap *= 2; \
                        char* _grown = realloc(buf, (size_t)bufCap); \
                        if (!_grown) goto str_oom; \
                        buf = _grown; \
                    } \
                    memcpy(buf + bufLen, (s), (size_t)_n); \
                    bufLen += _n; \
                    buf[bufLen] = '\0'; \
                } while (0)

                if (IS_LIST(v)) {
                    ObjList* list = AS_LIST(v);
                    SBUF_APPEND("[", 1);
                    for (int i = 0; i < list->count; i++) {
                        if (i) SBUF_APPEND(", ", 2);
                        Value item = list->items[i];
                        if (IS_STRING(item)) { SBUF_APPEND(AS_STRING(item)->chars, AS_STRING(item)->length); }
                        // ... nested list/dict omitted for simplicity unless user needs full recursive printing in str() ...
                        // If they do need it, this can be refactored to use a recursive helper.
                        // For exact behavior match, we can just use the print helper to a string.
                        else {
                            char tmp[64];
                            if (IS_NUMBER(item)) {
                                double n = AS_NUMBER(item);
                                if (n == (long long)n) snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
                                else snprintf(tmp, sizeof(tmp), "%g", n);
                            } else if (IS_BOOL(item)) {
                                snprintf(tmp, sizeof(tmp), "%s", AS_BOOL(item) ? "true" : "false");
                            } else if (IS_NIL(item)) {
                                snprintf(tmp, sizeof(tmp), "nil");
                            } else if (IS_CHAR(item)) {
                                snprintf(tmp, sizeof(tmp), "%c", AS_CHAR(item));
                            } else {
                                snprintf(tmp, sizeof(tmp), "<%s>", typeName(item));
                            }
                            SBUF_APPEND(tmp, strlen(tmp));
                        }
                    }
                    SBUF_APPEND("]", 1);
                } else if (IS_DICT(v)) {
                    ObjDict* dict = AS_DICT(v);
                    SBUF_APPEND("{", 1);
                    int printed = 0;
                    for (int i = 0; i < dict->capacity; i++) {
                        DictEntry* e = &dict->entries[i];
                        if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
                        if (printed++) SBUF_APPEND(", ", 2);
                        SBUF_APPEND("\"", 1);
                        SBUF_APPEND(e->key->chars, e->key->length);
                        SBUF_APPEND("\": ", 3);
                        if (IS_STRING(e->value)) {
                            SBUF_APPEND(AS_STRING(e->value)->chars, AS_STRING(e->value)->length);
                        } else {
                            char tmp[64];
                            if (IS_NUMBER(e->value)) {
                                double n = AS_NUMBER(e->value);
                                if (n == (long long)n) snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
                                else snprintf(tmp, sizeof(tmp), "%g", n);
                            } else if (IS_BOOL(e->value)) {
                                snprintf(tmp, sizeof(tmp), "%s", AS_BOOL(e->value) ? "true" : "false");
                            } else if (IS_NIL(e->value)) {
                                snprintf(tmp, sizeof(tmp), "nil");
                            } else {
                                snprintf(tmp, sizeof(tmp), "<%s>", typeName(e->value));
                            }
                            SBUF_APPEND(tmp, strlen(tmp));
                        }
                    }
                    SBUF_APPEND("}", 1);
                } else if (IS_SOCKET(v)) {
                    ObjSocket* sock = AS_SOCKET(v);
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "<socket %s>", sock->closed ? "closed" : (sock->isListening ? "listening" : "connected"));
                    SBUF_APPEND(tmp, strlen(tmp));
                } else {
                    const char* name = "<fn>";
                    if (IS_CLOSURE(v)) {
                        ObjFunction* fn = AS_CLOSURE(v)->function;
                        name = fn->name ? fn->name->chars : "<fn>";
                    } else if (IS_FUNCTION(v)) {
                        ObjFunction* fn = AS_FUNCTION(v);
                        name = fn->name ? fn->name->chars : "<fn>";
                    }
                    char tmp[128];
                    snprintf(tmp, sizeof(tmp), "<fn %s>", name);
                    SBUF_APPEND(tmp, strlen(tmp));
                }
                #undef SBUF_APPEND

                ObjString* result = copyString(buf, bufLen);
                free(buf);
                push((Value){VAL_OBJ,{.obj=(Obj*)result}});
                break;
                
                str_oom:
                free(buf);
                push((Value){VAL_OBJ,{.obj=(Obj*)copyString("<oom>",5)}});
            }
            break;
        }
        case BUILTIN_CHAR: {
            Value v = pop();
            if (IS_NUMBER(v)) push(CHAR_VAL((char)(int)AS_NUMBER(v)));
            else return raiseError("char() expects number");
            break;
        }
        case BUILTIN_ORD: {
            Value v = pop();
            if (IS_CHAR(v)) push(NUMBER_VAL((double)AS_CHAR(v)));
            else if (IS_STRING(v) && AS_STRING(v)->length > 0) push(NUMBER_VAL((double)(unsigned char)AS_STRING(v)->chars[0]));
            else return raiseError("ord() expects char or single-char string");
            break;
        }
       case BUILTIN_INPUT: {
    if (argc > 1) return raiseError("input() expects 0 or 1 argument");
    if (argc == 1) {
        Value prompt = pop();
        if (IS_STRING(prompt)) {
            printf("%s", AS_STRING(prompt)->chars);
            fflush(stdout);  // ← ito ang fix
        }
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        push(NIL_VAL);
        break;
    }
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
    push(OBJ_VAL(copyString(buf, len)));
    break;
}
        case BUILTIN_RANGE: {
            double step = 1.0, end = 0.0, start = 0.0;
            if (argc >= 3) {
                Value stepVal = pop(); Value endVal = pop(); Value startVal = pop();
                if (!IS_NUMBER(startVal) || !IS_NUMBER(endVal) || !IS_NUMBER(stepVal)) return raiseError("range() expects numbers");
                step = AS_NUMBER(stepVal); end = AS_NUMBER(endVal); start = AS_NUMBER(startVal);
            } else if (argc == 2) {
                Value ev = pop(), stv = pop();
                if (!IS_NUMBER(ev) || !IS_NUMBER(stv)) return raiseError("range() expects numbers");
                end = AS_NUMBER(ev); start = AS_NUMBER(stv);
            } else if (argc == 1) {
                Value ev = pop();
                if (!IS_NUMBER(ev)) return raiseError("range() expects a number");
                end = AS_NUMBER(ev);
            }
            ObjList* list = newList();
            if (step > 0) { for (double i = start; i < end; i += step) listAppend(list, NUMBER_VAL(i)); }
            else if (step < 0) { for (double i = start; i > end; i += step) listAppend(list, NUMBER_VAL(i)); }
            push((Value){VAL_OBJ, {.obj = (Obj*)list}});
            break;
        }
        case BUILTIN_SPAWN: {
            if (argc < 1) return raiseError("spawn() expects at least 1 argument (function)");
            int argCount = argc - 1;
            Value* args = NULL;
            if (argCount > 0) {
                args = (Value*)malloc(sizeof(Value) * (size_t)argCount);
                if (!args) return raiseError("spawn() out of memory");
                for (int i = argCount - 1; i >= 0; i--) args[i] = pop();
            }
            Value fnVal = pop();
            if (!IS_CLOSURE(fnVal)) { free(args); return raiseError("spawn() first argument must be a function"); }
            ObjClosure* closure = AS_CLOSURE(fnVal);
            if (closure->function->arity != argCount) {
                free(args);
                return raiseError("spawn() function expects %d argument(s), got %d", closure->function->arity, argCount);
            }
            Fiber* nf = newFiber();
            nf->stack[nf->stackCount++] = fnVal;
            for (int i = 0; i < argCount; i++) nf->stack[nf->stackCount++] = args[i];
            free(args);
            nf->frames[0].closure = closure;
            nf->frames[0].ip = closure->function->chunk.code;
            nf->frames[0].slotsIndex = 0; 
            nf->frameCount = 1;
            nf->state = FIBER_READY;
            enqueueReady(nf);
            push(NIL_VAL);
            break;
        }
        case BUILTIN_YIELD: {
            if (argc != 0) return raiseError("yield() takes no arguments");
            push(NIL_VAL);  
            if (vm.readyHead == NULL) break;
            Fiber* me = vm.current;
            me->state = FIBER_READY;
            enqueueReady(me);
            Fiber* next = dequeueReady();
            next->state = FIBER_RUNNING;
            vm.current = next;
            return INTERPRET_YIELD;
        }
        case BUILTIN_WAIT_ALL: {
            if (argc != 0) return raiseError("waitAll() takes no arguments");
            // No other fibers — done immediately.
            if (vm.readyHead == NULL && vm.blockedHead == NULL) {
                if (vm.anyFiberCrashed) {
                    vm.anyFiberCrashed = false;
                    Value err = vm.hasCrashError
                        ? vm.lastCrashError
                        : (Value){VAL_OBJ, {.obj = (Obj*)copyString("a spawned fiber crashed", 23)}};
                    vm.hasCrashError = false;
                    return raiseValue(err);
                }
                push(NIL_VAL);
                break;
            }
            // Other fibers still running or blocked on I/O.
            // Rewind ip by 3 bytes (opcode + id + argc) so that when main
            // resumes it re-executes waitAll() and checks again — making this
            // a spin-yield loop without any extra state.
            vm.current->frames[vm.current->frameCount - 1].ip -= 3;

            if (vm.readyHead != NULL) {
                // There are ready fibers — yield to them.
                Fiber* me = vm.current;
                me->state = FIBER_READY;
                enqueueReady(me);
                Fiber* next = dequeueReady();
                next->state = FIBER_RUNNING;
                vm.current = next;
                return INTERPRET_YIELD;
            } else {
                // All fibers are blocked on I/O — poll until at least one
                // unblocks, then yield to it.
                while (vm.readyHead == NULL && vm.blockedHead != NULL) {
                    pollBlockedFibers();
                }
                if (vm.readyHead != NULL) {
                    Fiber* me = vm.current;
                    me->state = FIBER_READY;
                    enqueueReady(me);
                    Fiber* next = dequeueReady();
                    next->state = FIBER_RUNNING;
                    vm.current = next;
                    return INTERPRET_YIELD;
                }
                // blockedHead is also NULL now — all done, fall through to push NIL.
                // Undo the ip rewind.
                vm.current->frames[vm.current->frameCount - 1].ip += 3;
                if (vm.anyFiberCrashed) {
                    vm.anyFiberCrashed = false;
                    Value err = vm.hasCrashError
                        ? vm.lastCrashError
                        : (Value){VAL_OBJ, {.obj = (Obj*)copyString("a spawned fiber crashed", 23)}};
                    vm.hasCrashError = false;
                    return raiseValue(err);
                }
                push(NIL_VAL);
            }
            break;
        }
        case BUILTIN_ARGS: {
            if (argc != 0) return raiseError("args() takes no arguments");
            ObjList* list = newList();
            for (int i = 0; i < g_scriptArgc; i++) {
                ObjString* s = copyString(g_scriptArgv[i], (int)strlen(g_scriptArgv[i]));
                listAppend(list, (Value){VAL_OBJ, {.obj = (Obj*)s}});
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)list}});
            break;
        }
        case BUILTIN_GETENV: {
            if (argc != 1) return raiseError("getEnv() expects 1 argument (name)");
            Value nameVal = pop();
            if (!IS_STRING(nameVal)) return raiseError("getEnv() expects a string name");
            const char* val = getenv(AS_STRING(nameVal)->chars);
            if (val == NULL) push(NIL_VAL);
            else {
                ObjString* s = copyString(val, (int)strlen(val));
                push((Value){VAL_OBJ, {.obj = (Obj*)s}});
            }
            break;
        }
        case BUILTIN_SCRIPT_PATH: {
            if (argc != 0) return raiseError("scriptPath() takes no arguments");
            if (g_scriptPath == NULL) push(NIL_VAL);
            else {
                ObjString* s = copyString(g_scriptPath, (int)strlen(g_scriptPath));
                push((Value){VAL_OBJ, {.obj = (Obj*)s}});
            }
            break;
        }
        default:
            return raiseError("Unknown Core builtin %d", id);
    }
    return INTERPRET_OK;
}

// Builtins missing handlers will just return a runtime error for now, 
// they'll be added in the networking extraction phase.
InterpretResult callBuiltin(uint8_t id, uint8_t argc) {
    if (id <= BUILTIN_RANGE) return callBuiltinCore(id, argc);
    if (id >= BUILTIN_FILE_READ && id <= BUILTIN_FILE_DELETE) return callBuiltinIO(id, argc);
    if (id >= BUILTIN_ARGS && id <= BUILTIN_SCRIPT_PATH) return callBuiltinCore(id, argc);
    if (id >= BUILTIN_SQRT && id <= BUILTIN_RANDOM_INT) return callBuiltinMath(id, argc);
    if (id >= BUILTIN_JSON_PARSE && id <= BUILTIN_JSON_STRINGIFY) return callBuiltinJSON(id, argc);
    if (id >= BUILTIN_SPAWN && id <= BUILTIN_WAIT_ALL) return callBuiltinCore(id, argc); // spawn, yield, waitAll
    
    if (id >= BUILTIN_HTTP_GET && id <= BUILTIN_HTTP_PATCH) return callBuiltinHTTP(id, argc);
    if (id >= BUILTIN_TCP_LISTEN && id <= BUILTIN_TCP_CLOSE) return callBuiltinTcp(id, argc);
    if (id >= BUILTIN_TLS_LISTEN && id <= BUILTIN_TLS_CLOSE) return callBuiltinTls(id, argc);
    if (id >= BUILTIN_HTTP_PARSE && id <= BUILTIN_SSE_WRITE) return callBuiltinServer(id, argc);
    if (id >= BUILTIN_WS_HANDSHAKE && id <= BUILTIN_WS_CLOSE) return callBuiltinWs(id, argc);
    if (id >= BUILTIN_TIME && id <= BUILTIN_DATE_MAKE) return callBuiltinTime(id, argc);
    if (id >= BUILTIN_REGEX_TEST && id <= BUILTIN_REGEX_SPLIT) return callBuiltinRegex(id, argc);

    return raiseError("Builtin %d not found", id);
}
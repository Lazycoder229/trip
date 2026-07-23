#include "tvm.h"

InterpretResult callMethodStr(ObjString* self, uint8_t id, uint8_t argc) {
    switch (id) {
        case METHOD_STR_LEN:
        case METHOD_LIST_LEN: { // list len is technically for lists, but the enum is reused or handled
            for (int i = 0; i < argc; i++) pop();
            pop(); // receiver
            push(NUMBER_VAL(self->length));
            break;
        }
        case METHOD_STR_UPPER: {
            char* buf = malloc(self->length + 1);
            for (int i = 0; i < self->length; i++) buf[i] = toupper(self->chars[i]);
            buf[self->length] = '\0';
            pop(); // receiver
            push((Value){VAL_OBJ, {.obj=(Obj*)copyString(buf, self->length)}});
            free(buf);
            break;
        }
        case METHOD_STR_LOWER: {
            char* buf = malloc(self->length + 1);
            for (int i = 0; i < self->length; i++) buf[i] = tolower(self->chars[i]);
            buf[self->length] = '\0';
            pop();
            push((Value){VAL_OBJ, {.obj=(Obj*)copyString(buf, self->length)}});
            free(buf);
            break;
        }
        case METHOD_STR_TRIM: {
            int s = 0, e = self->length > 0 ? self->length - 1 : -1;
            while (s <= e && isspace((unsigned char)self->chars[s])) s++;
            while (e >= s && isspace((unsigned char)self->chars[e])) e--;
            pop();
            int trimLen = (e >= s) ? (e - s + 1) : 0;
            push((Value){VAL_OBJ, {.obj=(Obj*)copyString(self->chars + s, trimLen)}});
            break;
        }
        case METHOD_STR_CONTAINS:
        case METHOD_LIST_CONTAINS: { // Reused enum? Handled by caller or same id.
            if (argc < 1) { return raiseError("contains() needs 1 arg"); }
            Value arg = pop(); pop(); // arg, receiver
            if (!IS_STRING(arg)) { push(BOOL_VAL(false)); break; }
            ObjString* sub = AS_STRING(arg);
            bool found = (sub->length == 0) || (strstr(self->chars, sub->chars) != NULL);
            push(BOOL_VAL(found));
            break;
        }
        case METHOD_STR_STARTS_WITH: {
            Value arg = pop(); pop();
            if (!IS_STRING(arg)) { push(BOOL_VAL(false)); break; }
            ObjString* pre = AS_STRING(arg);
            bool ok = self->length >= pre->length && memcmp(self->chars, pre->chars, pre->length) == 0;
            push(BOOL_VAL(ok));
            break;
        }
        case METHOD_STR_ENDS_WITH: {
            Value arg = pop(); pop();
            if (!IS_STRING(arg)) { push(BOOL_VAL(false)); break; }
            ObjString* suf = AS_STRING(arg);
            bool ok = self->length >= suf->length &&
                      memcmp(self->chars + self->length - suf->length, suf->chars, suf->length) == 0;
            push(BOOL_VAL(ok));
            break;
        }
        case METHOD_STR_REPLACE: {
            if (argc < 2) { return raiseError("replace() needs 2 args"); }
            Value rep = pop(); Value pat = pop(); pop();
            if (!IS_STRING(pat) || !IS_STRING(rep)) { push((Value){VAL_OBJ,{.obj=(Obj*)self}}); break; }
            ObjString* p = AS_STRING(pat); ObjString* r = AS_STRING(rep);
            size_t occurrences = 0;
            if (p->length > 0) {
                const char* scan = self->chars;
                while ((scan = strstr(scan, p->chars)) != NULL) {
                    occurrences++;
                    scan += p->length;
                }
            }
            size_t outLen = (size_t)self->length
                          - occurrences * (size_t)p->length
                          + occurrences * (size_t)r->length;
            char* result = malloc(outLen + 1);
            if (!result) {
                push((Value){VAL_OBJ,{.obj=(Obj*)self}}); 
                break;
            }
            char* src = self->chars;
            char* found_p;
            char* dst = result;
            if (p->length > 0) {
                while ((found_p = strstr(src, p->chars))) {
                    size_t before = (size_t)(found_p - src);
                    memcpy(dst, src, before); dst += before;
                    memcpy(dst, r->chars, (size_t)r->length); dst += r->length;
                    src = found_p + p->length;
                }
            }
            size_t tail = strlen(src);
            memcpy(dst, src, tail);
            dst[tail] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(result, (int)outLen)}});
            free(result);
            break;
        }
        case METHOD_STR_SLICE:
        case METHOD_LIST_SLICE: {
            Value end_v = (argc >= 2) ? pop() : NUMBER_VAL(self->length);
            Value start_v = (argc >= 1) ? pop() : NUMBER_VAL(0);
            pop();
            if (!IS_NUMBER(start_v) || !IS_NUMBER(end_v)) { return raiseError("slice() bounds must be numbers"); }
            int s = (int)AS_NUMBER(start_v);
            int e = (int)AS_NUMBER(end_v);
            if (s < 0) s = self->length + s;
            if (e < 0) e = self->length + e;
            if (s < 0) s = 0;
            if (e > self->length) e = self->length;
            if (s >= e) push((Value){VAL_OBJ,{.obj=(Obj*)copyString("",0)}});
            else push((Value){VAL_OBJ,{.obj=(Obj*)copyString(self->chars + s, e - s)}});
            break;
        }
        case METHOD_STR_FIND: {
            Value arg = pop(); pop();
            if (!IS_STRING(arg)) { push(NUMBER_VAL(-1)); break; }
            char* found_p = strstr(self->chars, AS_STRING(arg)->chars);
            push(NUMBER_VAL(found_p ? (found_p - self->chars) : -1));
            break;
        }
        case METHOD_STR_REPEAT: {
            Value n_v = pop(); pop();
            if (!IS_NUMBER(n_v)) { return raiseError("repeat() count must be a number"); }
            int n = (int)AS_NUMBER(n_v);
            if (n <= 0) { push((Value){VAL_OBJ,{.obj=(Obj*)copyString("",0)}}); break; }
            if (self->length > 0 && n > (INT_MAX - 1) / self->length) {
                return raiseError("repeat() result would be too large");
            }
            int len = self->length * n;
            char* buf = malloc((size_t)len + 1);
            if (!buf) { return raiseError("Out of memory in repeat()"); }
            for (int i = 0; i < n; i++) memcpy(buf + i * self->length, self->chars, self->length);
            buf[len] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf, len)}});
            free(buf);
            break;
        }
        case METHOD_STR_SPLIT: {
            ObjString* delim = NULL;
            if (argc >= 1) {
                Value dv = pop();
                if (IS_STRING(dv)) delim = AS_STRING(dv);
            }
            pop(); // receiver
            ObjList* list = newList();
            if (!delim || delim->length == 0) {
                for (int i = 0; i < self->length; i++) {
                    listAppend(list, (Value){VAL_OBJ,{.obj=(Obj*)copyString(self->chars+i, 1)}});
                }
            } else {
                char* src = self->chars; char* found_p;
                while ((found_p = strstr(src, delim->chars))) {
                    listAppend(list, (Value){VAL_OBJ,{.obj=(Obj*)copyString(src, found_p - src)}});
                    src = found_p + delim->length;
                }
                listAppend(list, (Value){VAL_OBJ,{.obj=(Obj*)copyString(src, strlen(src))}});
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)list}});
            break;
        }
        case METHOD_STR_CHARS: {
            pop(); // receiver
            ObjList* list = newList();
            for (int i = 0; i < self->length; i++)
                listAppend(list, (Value){VAL_OBJ,{.obj=(Obj*)copyString(self->chars+i,1)}});
            push((Value){VAL_OBJ,{.obj=(Obj*)list}});
            break;
        }
        case METHOD_STR_REVERSE: {
            pop();
            char* buf = malloc(self->length + 1);
            for (int i = 0; i < self->length; i++)
                buf[i] = self->chars[self->length - 1 - i];
            buf[self->length] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf, self->length)}});
            free(buf);
            break;
        }
        case METHOD_STR_PAD_LEFT: {
            ObjString* pad = NULL;
            if (argc >= 2) { Value pv = pop(); if (IS_STRING(pv)) pad = AS_STRING(pv); }
            Value n_v = pop(); pop(); // width, receiver
            if (!IS_NUMBER(n_v)) { return raiseError("padLeft() width must be a number"); }
            int width = (int)AS_NUMBER(n_v);
            if (width <= self->length) { push((Value){VAL_OBJ,{.obj=(Obj*)self}}); break; }
            int padLen = width - self->length;
            char padChar = (pad && pad->length > 0) ? pad->chars[0] : ' ';
            char* buf = malloc(width + 1);
            memset(buf, padChar, padLen);
            memcpy(buf + padLen, self->chars, self->length);
            buf[width] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf, width)}});
            free(buf);
            break;
        }
        case METHOD_STR_PAD_RIGHT: {
            ObjString* pad = NULL;
            if (argc >= 2) { Value pv = pop(); if (IS_STRING(pv)) pad = AS_STRING(pv); }
            Value n_v = pop(); pop();
            if (!IS_NUMBER(n_v)) { return raiseError("padRight() width must be a number"); }
            int width = (int)AS_NUMBER(n_v);
            if (width <= self->length) { push((Value){VAL_OBJ,{.obj=(Obj*)self}}); break; }
            char padChar = (pad && pad->length > 0) ? pad->chars[0] : ' ';
            char* buf = malloc(width + 1);
            memcpy(buf, self->chars, self->length);
            memset(buf + self->length, padChar, width - self->length);
            buf[width] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf, width)}});
            free(buf);
            break;
        }
        case METHOD_STR_FORMAT: {
            Value* args = (Value*)malloc(sizeof(Value) * argc);
            for (int i = argc - 1; i >= 0; i--) args[i] = pop();
            pop(); // receiver

            int argIdx = 0;
            size_t outCap = self->length + 64;
            char* out = malloc(outCap);
            int outLen = 0;
            for (int i = 0; i < self->length; i++) {
                if (self->chars[i] == '{' && i+1 < self->length && self->chars[i+1] == '}') {
                    char tmp[64]; const char* rep; int repLen;
                    if (argIdx < argc) {
                        Value av = args[argIdx++];
                        if (IS_STRING(av)) { rep = AS_STRING(av)->chars; repLen = AS_STRING(av)->length; }
                        else if (IS_NUMBER(av)) {
                            double n = AS_NUMBER(av);
                            if (n == (long long)n) snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
                            else snprintf(tmp, sizeof(tmp), "%g", n);
                            rep = tmp; repLen = strlen(tmp);
                        } else if (IS_BOOL(av)) { rep = AS_BOOL(av) ? "true" : "false"; repLen = strlen(rep); }
                        else if (IS_NIL(av)) { rep = "nil"; repLen = 3; }
                        else { rep = "?"; repLen = 1; }
                    } else { rep = "{}"; repLen = 2; }
                    while (outLen + repLen + 1 >= (int)outCap) { outCap *= 2; out = realloc(out, outCap); }
                    memcpy(out + outLen, rep, repLen); outLen += repLen;
                    i++; // skip '}'
                } else {
                    if (outLen + 2 >= (int)outCap) { outCap *= 2; out = realloc(out, outCap); }
                    out[outLen++] = self->chars[i];
                }
            }
            out[outLen] = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(out, outLen)}});
            free(out); free(args);
            break;
        }
        case METHOD_STR_IS_DIGIT: {
            pop();
            bool ok = self->length > 0;
            for (int i = 0; i < self->length && ok; i++)
                if (!isdigit((unsigned char)self->chars[i])) ok = false;
            push(BOOL_VAL(ok));
            break;
        }
        case METHOD_STR_IS_ALPHA: {
            pop();
            bool ok = self->length > 0;
            for (int i = 0; i < self->length && ok; i++)
                if (!isalpha((unsigned char)self->chars[i])) ok = false;
            push(BOOL_VAL(ok));
            break;
        }
        case METHOD_STR_IS_UPPER: {
            pop();
            bool ok = self->length > 0;
            for (int i = 0; i < self->length && ok; i++)
                if (!isupper((unsigned char)self->chars[i]) && isalpha((unsigned char)self->chars[i])) ok = false;
            push(BOOL_VAL(ok));
            break;
        }
        case METHOD_STR_IS_LOWER: {
            pop();
            bool ok = self->length > 0;
            for (int i = 0; i < self->length && ok; i++)
                if (!islower((unsigned char)self->chars[i]) && isalpha((unsigned char)self->chars[i])) ok = false;
            push(BOOL_VAL(ok));
            break;
        }
        default:
            return raiseError("Unknown string method %d", id);
    }
    return INTERPRET_OK;
}

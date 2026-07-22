#include "tvm.h"

InterpretResult callMethodList(ObjList* self, uint8_t id, uint8_t argc) {
    switch (id) {
        case METHOD_LIST_LEN: { pop(); push(NUMBER_VAL(self->count)); break; }
        case METHOD_LIST_APPEND: {
            Value item = pop(); pop();
            listAppend(self, item);
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_PUSH: {
            Value item = pop(); pop();
            listAppend(self, item);
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_POP: {
            pop(); // receiver
            if (self->count == 0) { return raiseError("pop() on empty list"); }
            push(self->items[--self->count]);
            break;
        }
        case METHOD_LIST_INSERT: {
            Value item = pop(); Value idx_v = pop(); pop();
            if (!IS_NUMBER(idx_v)) { return raiseError("insert() index must be a number"); }
            int idx = (int)AS_NUMBER(idx_v);
            if (idx < 0) idx = self->count + idx;
            if (idx < 0) idx = 0;
            if (idx > self->count) idx = self->count;
            listAppend(self, NIL_VAL); // grow
            memmove(&self->items[idx+1], &self->items[idx], sizeof(Value)*(self->count-1-idx));
            self->items[idx] = item;
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_REMOVE: {
            Value idx_v = pop(); pop();
            if (!IS_NUMBER(idx_v)) { return raiseError("remove() index must be a number"); }
            int idx = (int)AS_NUMBER(idx_v);
            if (idx < 0) idx = self->count + idx;
            if (idx < 0 || idx >= self->count) { return raiseError("list index out of range"); }
            Value removed = self->items[idx];
            memmove(&self->items[idx], &self->items[idx+1], sizeof(Value)*(self->count-idx-1));
            self->count--;
            push(removed);
            break;
        }
        case METHOD_LIST_CONTAINS: {
            Value item = pop(); pop();
            bool found = false;
            for (int i = 0; i < self->count && !found; i++)
                if (valuesEqual(self->items[i], item)) found = true;
            push(BOOL_VAL(found));
            break;
        }
        case METHOD_LIST_REVERSE: {
            pop();
            for (int i = 0, j = self->count-1; i < j; i++, j--) {
                Value tmp = self->items[i]; self->items[i] = self->items[j]; self->items[j] = tmp;
            }
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_SORT: {
            pop();
            for (int i = 1; i < self->count; i++) {
                Value key = self->items[i]; int j = i - 1;
                while (j >= 0) {
                    bool gt = false;
                    if (IS_NUMBER(self->items[j]) && IS_NUMBER(key)) gt = AS_NUMBER(self->items[j]) > AS_NUMBER(key);
                    else if (IS_STRING(self->items[j]) && IS_STRING(key))
                        gt = strcmp(AS_STRING(self->items[j])->chars, AS_STRING(key)->chars) > 0;
                    if (!gt) break;
                    self->items[j+1] = self->items[j]; j--;
                }
                self->items[j+1] = key;
            }
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_SLICE: {
            Value end_v = (argc >= 2) ? pop() : NUMBER_VAL(self->count);
            Value start_v = (argc >= 1) ? pop() : NUMBER_VAL(0);
            pop();
            if (!IS_NUMBER(start_v) || !IS_NUMBER(end_v)) { return raiseError("slice() bounds must be numbers"); }
            int s = (int)AS_NUMBER(start_v), e = (int)AS_NUMBER(end_v);
            if (s < 0) s = self->count + s;
            if (e < 0) e = self->count + e;
            if (s < 0) s = 0;
            if (e < 0) e = 0;
            if (e > self->count) e = self->count;
            ObjList* result = newList();
            for (int i = s; i < e; i++) listAppend(result, self->items[i]);
            push((Value){VAL_OBJ,{.obj=(Obj*)result}});
            break;
        }
        case METHOD_LIST_JOIN: {
            ObjString* sep = NULL;
            if (argc >= 1) { Value sv = pop(); if (IS_STRING(sv)) sep = AS_STRING(sv); }
            pop();
            int total = 0;
            for (int i = 0; i < self->count; i++) {
                if (IS_STRING(self->items[i])) total += AS_STRING(self->items[i])->length;
                if (i < self->count-1 && sep) total += sep->length;
            }
            char* buf = malloc(total + 1); char* dst = buf;
            for (int i = 0; i < self->count; i++) {
                if (IS_STRING(self->items[i])) {
                    ObjString* s = AS_STRING(self->items[i]);
                    memcpy(dst, s->chars, s->length); dst += s->length;
                }
                if (i < self->count-1 && sep) { memcpy(dst, sep->chars, sep->length); dst += sep->length; }
            }
            *dst = '\0';
            push((Value){VAL_OBJ,{.obj=(Obj*)copyString(buf, total)}});
            free(buf);
            break;
        }
        case METHOD_LIST_CLEAR: {
            pop(); self->count = 0; push(NIL_VAL); break;
        }
        case METHOD_LIST_MAP: {
            if (argc < 1) return raiseError("map() needs 1 argument (function)");
            Value fn_val = pop(); pop(); // fn, receiver
            if (!IS_CLOSURE(fn_val)) return raiseError("map() argument must be a function");
            ObjClosure* closure = AS_CLOSURE(fn_val);
            if (closure->function->arity != 1) return raiseError("map() callback must take 1 argument");
            ObjList* result = newList();
            for (int i = 0; i < self->count; i++) {
                push((Value){VAL_OBJ, {.obj = (Obj*)closure}});
                push(self->items[i]);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure   = closure;
                newFrame->ip        = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - 2; 
                InterpretResult r = run();
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                listAppend(result, pop());
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }
        case METHOD_LIST_FILTER: {
            if (argc < 1) return raiseError("filter() needs 1 argument (function)");
            Value fn_val = pop(); pop(); 
            if (!IS_CLOSURE(fn_val)) return raiseError("filter() argument must be a function");
            ObjClosure* closure = AS_CLOSURE(fn_val);
            if (closure->function->arity != 1) return raiseError("filter() callback must take 1 argument");
            ObjList* result = newList();
            for (int i = 0; i < self->count; i++) {
                push((Value){VAL_OBJ, {.obj = (Obj*)closure}});
                push(self->items[i]);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure   = closure;
                newFrame->ip        = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - 2;
                InterpretResult r = run();
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                Value pred = pop();
                if (!isFalsy(pred)) listAppend(result, self->items[i]);
            }
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }
        case METHOD_LIST_REDUCE: {
            if (argc < 2) return raiseError("reduce() needs 2 arguments (fn, initial)");
            Value init_v = pop(); Value fn_val = pop(); pop(); 
            if (!IS_CLOSURE(fn_val)) return raiseError("reduce() first argument must be a function");
            ObjClosure* closure = AS_CLOSURE(fn_val);
            if (closure->function->arity != 2) return raiseError("reduce() callback must take 2 arguments (acc, item)");
            Value acc = init_v;
            for (int i = 0; i < self->count; i++) {
                push((Value){VAL_OBJ, {.obj = (Obj*)closure}});
                push(acc);
                push(self->items[i]);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure    = closure;
                newFrame->ip         = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - 3;
                InterpretResult r = run();
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                acc = pop();
            }
            push(acc);
            break;
        }
        case METHOD_LIST_FOR_EACH: {
            if (argc < 1) return raiseError("forEach() needs 1 argument (function)");
            Value fn_val = pop(); pop(); 
            if (!IS_CLOSURE(fn_val)) return raiseError("forEach() argument must be a function");
            ObjClosure* closure = AS_CLOSURE(fn_val);
            if (closure->function->arity != 1) return raiseError("forEach() callback must take 1 argument");
            for (int i = 0; i < self->count; i++) {
                push((Value){VAL_OBJ, {.obj = (Obj*)closure}});
                push(self->items[i]);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure    = closure;
                newFrame->ip         = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - 2;
                InterpretResult r = run();
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                pop(); 
            }
            push(NIL_VAL);
            break;
        }
        case METHOD_LIST_INDEX_OF: {
            if (argc < 1) return raiseError("indexOf() needs 1 argument");
            Value target = pop(); pop(); 
            int found = -1;
            for (int i = 0; i < self->count && found < 0; i++)
                if (valuesEqual(self->items[i], target)) found = i;
            push(NUMBER_VAL(found));
            break;
        }
        case METHOD_LIST_FLATTEN: {
            pop(); // receiver
            ObjList* result = newList();
            for (int i = 0; i < self->count; i++) {
                if (IS_LIST(self->items[i])) {
                    ObjList* inner = AS_LIST(self->items[i]);
                    for (int j = 0; j < inner->count; j++)
                        listAppend(result, inner->items[j]);
                } else {
                    listAppend(result, self->items[i]);
                }
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)result}});
            break;
        }
        case METHOD_LIST_ZIP: {
            if (argc < 1) return raiseError("zip() needs 1 argument");
            Value other_v = pop(); pop(); 
            if (!IS_LIST(other_v)) return raiseError("zip() argument must be a list");
            ObjList* other = AS_LIST(other_v);
            int len = self->count < other->count ? self->count : other->count;
            ObjList* result = newList();
            for (int i = 0; i < len; i++) {
                ObjList* pair = newList();
                listAppend(pair, self->items[i]);
                listAppend(pair, other->items[i]);
                listAppend(result, (Value){VAL_OBJ,{.obj=(Obj*)pair}});
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)result}});
            break;
        }
        case METHOD_LIST_ENUMERATE: {
            pop(); // receiver
            ObjList* result = newList();
            for (int i = 0; i < self->count; i++) {
                ObjList* pair = newList();
                listAppend(pair, NUMBER_VAL(i));
                listAppend(pair, self->items[i]);
                listAppend(result, (Value){VAL_OBJ,{.obj=(Obj*)pair}});
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)result}});
            break;
        }
        default:
            return raiseError("Unknown list method %d", id);
    }
    return INTERPRET_OK;
}

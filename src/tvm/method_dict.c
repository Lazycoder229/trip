#include "tvm.h"

InterpretResult callMethodDict(ObjDict* self, uint8_t id, uint8_t argc) {
    switch (id) {
        case METHOD_DICT_LEN:
        case METHOD_LIST_LEN: { pop(); push(NUMBER_VAL(self->count)); break; }
        case METHOD_DICT_GET: {
            Value def = (argc >= 2) ? pop() : NIL_VAL;
            Value key_v = pop(); pop();
            if (!IS_STRING(key_v)) { push(def); break; }
            Value out; bool found = dictGet(self, AS_STRING(key_v), &out);
            push(found ? out : def);
            break;
        }
        case METHOD_DICT_SET: {
            Value val = pop(); Value key_v = pop(); pop();
            if (!IS_STRING(key_v)) { return raiseError("dict key must be string"); }
            dictSet(self, AS_STRING(key_v), val);
            push(NIL_VAL);
            break;
        }
        case METHOD_DICT_DEL: {
            Value key_v = pop(); pop();
            if (!IS_STRING(key_v)) { push(BOOL_VAL(false)); break; }
            push(BOOL_VAL(dictDelete(self, AS_STRING(key_v))));
            break;
        }
        case METHOD_DICT_HAS: {
            Value key_v = pop(); pop();
            if (!IS_STRING(key_v)) { push(BOOL_VAL(false)); break; }
            Value dummy; push(BOOL_VAL(dictGet(self, AS_STRING(key_v), &dummy)));
            break;
        }
        case METHOD_DICT_KEYS: {
            pop();
            ObjList* list = newList();
            for (int i = 0; i < self->capacity; i++) {
                DictEntry* e = &self->entries[i];
                if (e->key && !IS_DICT_TOMBSTONE(e))
                    listAppend(list, (Value){VAL_OBJ,{.obj=(Obj*)e->key}});
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)list}});
            break;
        }
        case METHOD_DICT_VALUES: {
            pop();
            ObjList* list = newList();
            for (int i = 0; i < self->capacity; i++) {
                DictEntry* e = &self->entries[i];
                if (e->key && !IS_DICT_TOMBSTONE(e)) listAppend(list, e->value);
            }
            push((Value){VAL_OBJ,{.obj=(Obj*)list}});
            break;
        }
        case METHOD_DICT_CLEAR:
        case METHOD_LIST_CLEAR: {
            pop();
            for (int i = 0; i < self->capacity; i++) self->entries[i].key = NULL;
            self->count = 0;
            push(NIL_VAL);
            break;
        }
        default:
            return raiseError("Unknown dict method %d", id);
    }
    return INTERPRET_OK;
}

// ── method dispatch ─────────────────────────────────────────────────────────
InterpretResult callMethod(uint8_t id, uint8_t argc) {
    Value receiver = peek(argc);

    if (IS_STRING(receiver)) {
        return callMethodStr(AS_STRING(receiver), id, argc);
    }

    if (IS_LIST(receiver)) {
        return callMethodList(AS_LIST(receiver), id, argc);
    }

    if (IS_DICT(receiver)) {
        return callMethodDict(AS_DICT(receiver), id, argc);
    }

    return raiseError("Value of type '%s' has no methods", typeName(receiver));
}

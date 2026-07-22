#include "tvm.h"
#include "../cJSON.h"

// ── JSON Helpers ────────────────────────────────────────────────────────
Value cjsonToTrip(cJSON* node) {
    if (!node) return NIL_VAL;
    if (cJSON_IsNull(node))   return NIL_VAL;
    if (cJSON_IsTrue(node))   return BOOL_VAL(true);
    if (cJSON_IsFalse(node))  return BOOL_VAL(false);
    if (cJSON_IsNumber(node)) return NUMBER_VAL(node->valuedouble);
    if (cJSON_IsString(node)) {
        ObjString* s = copyString(node->valuestring, (int)strlen(node->valuestring));
        return (Value){VAL_OBJ, {.obj = (Obj*)s}};
    }
    if (cJSON_IsArray(node)) {
        ObjList* list = newList();
        push((Value){VAL_OBJ, {.obj = (Obj*)list}}); // GC guard
        cJSON* item = node->child;
        while (item) {
            Value val = cjsonToTrip(item);
            push(val); // GC guard
            listAppend(list, val);
            pop();
            item = item->next;
        }
        pop(); // list
        return (Value){VAL_OBJ, {.obj = (Obj*)list}};
    }
    if (cJSON_IsObject(node)) {
        ObjDict* dict = newDict();
        push((Value){VAL_OBJ, {.obj = (Obj*)dict}}); // GC guard
        cJSON* item = node->child;
        while (item) {
            ObjString* key = copyString(item->string, (int)strlen(item->string));
            push((Value){VAL_OBJ, {.obj = (Obj*)key}}); // GC guard
            Value val = cjsonToTrip(item);
            push(val);                                   // GC guard
            dictSet(dict, key, val);
            pop(); pop();                                // val, key
            item = item->next;
        }
        pop(); // dict
        return (Value){VAL_OBJ, {.obj = (Obj*)dict}};
    }
    return NIL_VAL;
}

cJSON* tripToCJSON(Value val) {
    if (IS_NIL(val))    return cJSON_CreateNull();
    if (IS_BOOL(val))   return AS_BOOL(val) ? cJSON_CreateTrue() : cJSON_CreateFalse();
    if (IS_NUMBER(val)) return cJSON_CreateNumber(AS_NUMBER(val));
    if (IS_STRING(val)) return cJSON_CreateString(AS_STRING(val)->chars);
    if (IS_LIST(val)) {
        ObjList* list = AS_LIST(val);
        cJSON* arr = cJSON_CreateArray();
        for (int i = 0; i < list->count; i++) {
            cJSON* child = tripToCJSON(list->items[i]);
            if (!child) child = cJSON_CreateNull();
            cJSON_AddItemToArray(arr, child);
        }
        return arr;
    }
    if (IS_DICT(val)) {
        ObjDict* dict = AS_DICT(val);
        cJSON* obj = cJSON_CreateObject();
        for (int i = 0; i < dict->capacity; i++) {
            DictEntry* e = &dict->entries[i];
            if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
            cJSON* child = tripToCJSON(e->value);
            if (!child) child = cJSON_CreateNull();
            cJSON_AddItemToObject(obj, e->key->chars, child);
        }
        return obj;
    }
    return NULL; // closures/functions not serializable
}

// ── Builtins ────────────────────────────────────────────────────────
InterpretResult callBuiltinJSON(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_JSON_PARSE: {
            if (argc != 1) return raiseError("jsonParse() expects 1 argument");
            Value arg = pop();
            if (!IS_STRING(arg)) return raiseError("jsonParse() expects a string");
            const char* src = AS_STRING(arg)->chars;

            cJSON* root = cJSON_Parse(src);
            if (!root) {
                const char* err = cJSON_GetErrorPtr();
                return raiseError("jsonParse() failed: %s", err ? err : "invalid JSON");
            }
            Value result = cjsonToTrip(root);
            cJSON_Delete(root);
            push(result);
            break;
        }

        case BUILTIN_JSON_STRINGIFY: {
            if (argc != 1) return raiseError("jsonStringify() expects 1 argument");
            Value arg = pop();

            cJSON* node = tripToCJSON(arg);
            if (!node) return raiseError("jsonStringify() failed: unsupported value");

            char* str = cJSON_PrintUnformatted(node);
            cJSON_Delete(node);
            if (!str) return raiseError("jsonStringify() failed: out of memory");

            ObjString* result = copyString(str, (int)strlen(str));
            free(str);
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }
        
        default:
            return raiseError("Unknown JSON builtin %d", id);
    }
    return INTERPRET_OK;
}

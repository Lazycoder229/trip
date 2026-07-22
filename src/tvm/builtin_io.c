#include "tvm.h"

// In original code, resolveFilePath was defined. We will assume it's in tvm.h or we define it here if it's missing.
// For now we'll assume it's in tvm.h or vm_helpers.c (we didn't extract it but we might need to).
// Let's implement resolveFilePath if it was in vm.c

InterpretResult callBuiltinIO(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_FILE_READ: {
            if (argc != 1) {
                return raiseError("readFile() expects 1 argument (path)");
            }
            Value pathVal = pop();
            if (!IS_STRING(pathVal)) {
                return raiseError("readFile() expects a string path");
            }
            const char* rawPath = AS_STRING(pathVal)->chars;
            char* path = resolveFilePath(rawPath);
            if (!path) {
                return raiseError("readFile(): path \"%s\" is outside the allowed directory", rawPath);
            }
            FILE* f = fopen(path, "rb");
            if (!f) {
                free(path);
                return raiseError("Could not open file \"%s\"", rawPath);
            }
            if (fseek(f, 0L, SEEK_END) != 0) {
                fclose(f); free(path);
                return raiseError("readFile(): seek failed on \"%s\"", rawPath);
            }
            long size = ftell(f);
            if (size < 0) {
                fclose(f); free(path);
                return raiseError("readFile(): could not determine size of \"%s\"", rawPath);
            }
            rewind(f);
            char* buf = malloc((size_t)size + 1);
            if (!buf) {
                fclose(f); free(path);
                return raiseError("readFile(): out of memory reading \"%s\"", rawPath);
            }
            size_t bytesRead = fread(buf, sizeof(char), (size_t)size, f);
            buf[bytesRead] = '\0';
            fclose(f);
            free(path);
            ObjString* s = copyString(buf, (int)bytesRead);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)s}});
            break;
        }
        case BUILTIN_FILE_WRITE:
        case BUILTIN_FILE_APPEND: {
            if (argc != 2) {
                return raiseError("%s() expects 2 arguments (path, content)",
                       id == BUILTIN_FILE_WRITE ? "writeFile" : "appendFile");
            }
            Value contentVal = pop();
            Value pathVal = pop();
            if (!IS_STRING(pathVal) || !IS_STRING(contentVal)) {
                return raiseError("%s() expects (string path, string content)",
                       id == BUILTIN_FILE_WRITE ? "writeFile" : "appendFile");
            }
            const char* rawPath = AS_STRING(pathVal)->chars;
            char* path = resolveFilePath(rawPath);
            if (!path) {
                return raiseError("%s(): path \"%s\" is outside the allowed directory",
                       id == BUILTIN_FILE_WRITE ? "writeFile" : "appendFile", rawPath);
            }
            FILE* f = fopen(path, id == BUILTIN_FILE_WRITE ? "wb" : "ab");
            if (!f) {
                free(path);
                return raiseError("Could not open file \"%s\" for writing", rawPath);
            }
            ObjString* content = AS_STRING(contentVal);
            size_t written = fwrite(content->chars, sizeof(char), (size_t)content->length, f);
            int writeOk = (written == (size_t)content->length);
            fclose(f);
            free(path);
            if (!writeOk) {
                return raiseError("%s(): write error on \"%s\"",
                       id == BUILTIN_FILE_WRITE ? "writeFile" : "appendFile", rawPath);
            }
            push(BOOL_VAL(true));
            break;
        }
        case BUILTIN_FILE_EXISTS: {
            if (argc != 1) {
                return raiseError("fileExists() expects 1 argument (path)");
            }
            Value pathVal = pop();
            if (!IS_STRING(pathVal)) {
                return raiseError("fileExists() expects a string path");
            }
            char* path = resolveFilePath(AS_STRING(pathVal)->chars);
            if (!path) { push(BOOL_VAL(false)); break; }
            FILE* f = fopen(path, "rb");
            free(path);
            if (f) {
                fclose(f);
                push(BOOL_VAL(true));
            } else {
                push(BOOL_VAL(false));
            }
            break;
        }
        case BUILTIN_FILE_DELETE: {
            if (argc != 1) {
                return raiseError("deleteFile() expects 1 argument (path)");
            }
            Value pathVal = pop();
            if (!IS_STRING(pathVal)) {
                return raiseError("deleteFile() expects a string path");
            }
            char* path = resolveFilePath(AS_STRING(pathVal)->chars);
            if (!path) { push(BOOL_VAL(false)); break; }
            int rc = remove(path);
            free(path);
            push(BOOL_VAL(rc == 0));
            break;
        }
        default:
            return raiseError("Unknown IO builtin %d", id);
    }
    return INTERPRET_OK;
}

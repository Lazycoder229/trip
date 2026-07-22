#include "tvm.h"
#include <string.h>
#include <stdlib.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#define PCRE2_COMPILE(pat_str, re_var) \
    int          _errcode;             \
    PCRE2_SIZE   _erroffset;           \
    pcre2_code*  re_var = pcre2_compile(                                    \
        (PCRE2_SPTR)(pat_str), PCRE2_ZERO_TERMINATED,                      \
        PCRE2_UTF, &_errcode, &_erroffset, NULL);                           \
    if (!(re_var)) {                                                         \
        PCRE2_UCHAR _ebuf[256];                                             \
        pcre2_get_error_message(_errcode, _ebuf, sizeof(_ebuf));            \
        char _errmsg[320];                                                  \
        snprintf(_errmsg, sizeof(_errmsg),                                  \
                 "regex compile error at offset %zu: %s",                   \
                 (size_t)_erroffset, (char*)_ebuf);                         \
        return raiseError(_errmsg);                                         \
    }

InterpretResult callBuiltinRegex(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_REGEX_TEST: {
            if (argc != 2) return raiseError("regexTest() expects 2 arguments (string, pattern)");
            Value patVal = pop();
            Value strVal = pop();
            if (!IS_STRING(strVal)) return raiseError("regexTest() first argument must be a string");
            if (!IS_STRING(patVal)) return raiseError("regexTest() second argument must be a string");
            const char* subject = AS_STRING(strVal)->chars;
            int         subjLen = AS_STRING(strVal)->length;
            const char* pattern = AS_STRING(patVal)->chars;

            PCRE2_COMPILE(pattern, re);
            pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
            int rc = pcre2_match(re, (PCRE2_SPTR)subject, (PCRE2_SIZE)subjLen,
                                 0, 0, md, NULL);
            pcre2_match_data_free(md);
            pcre2_code_free(re);
            push(BOOL_VAL(rc >= 0));
            break;
        }

        case BUILTIN_REGEX_MATCH: {
            if (argc != 2) return raiseError("regexMatch() expects 2 arguments (string, pattern)");
            Value patVal = pop();
            Value strVal = pop();
            if (!IS_STRING(strVal)) return raiseError("regexMatch() first argument must be a string");
            if (!IS_STRING(patVal)) return raiseError("regexMatch() second argument must be a string");
            const char* subject = AS_STRING(strVal)->chars;
            int         subjLen = AS_STRING(strVal)->length;
            const char* pattern = AS_STRING(patVal)->chars;

            PCRE2_COMPILE(pattern, re);
            pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
            int rc = pcre2_match(re, (PCRE2_SPTR)subject, (PCRE2_SIZE)subjLen,
                                 0, 0, md, NULL);
            if (rc < 0) {
                pcre2_match_data_free(md);
                pcre2_code_free(re);
                push(NIL_VAL);
                break;
            }
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
            uint32_t captureCount = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &captureCount);
            ObjList* result = newList();
            for (uint32_t i = 0; i <= captureCount; i++) {
                PCRE2_SIZE start = ov[2 * i];
                PCRE2_SIZE end   = ov[2 * i + 1];
                if (start == PCRE2_UNSET) {
                    listAppend(result, NIL_VAL);
                } else {
                    ObjString* s = copyString(subject + start, (int)(end - start));
                    listAppend(result, (Value){VAL_OBJ, {.obj = (Obj*)s}});
                }
            }
            pcre2_match_data_free(md);
            pcre2_code_free(re);
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }

        case BUILTIN_REGEX_MATCH_ALL: {
            if (argc != 2) return raiseError("regexMatchAll() expects 2 arguments (string, pattern)");
            Value patVal = pop();
            Value strVal = pop();
            if (!IS_STRING(strVal)) return raiseError("regexMatchAll() first argument must be a string");
            if (!IS_STRING(patVal)) return raiseError("regexMatchAll() second argument must be a string");
            const char* subject = AS_STRING(strVal)->chars;
            int         subjLen = AS_STRING(strVal)->length;
            const char* pattern = AS_STRING(patVal)->chars;

            PCRE2_COMPILE(pattern, re);
            uint32_t captureCount = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &captureCount);

            ObjList* allMatches = newList();
            PCRE2_SIZE offset = 0;

            while (offset <= (PCRE2_SIZE)subjLen) {
                pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
                int rc = pcre2_match(re, (PCRE2_SPTR)subject, (PCRE2_SIZE)subjLen,
                                     offset, 0, md, NULL);
                if (rc < 0) {
                    pcre2_match_data_free(md);
                    break;
                }
                PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
                ObjList* oneMatch = newList();
                for (uint32_t i = 0; i <= captureCount; i++) {
                    PCRE2_SIZE s = ov[2 * i];
                    PCRE2_SIZE e = ov[2 * i + 1];
                    if (s == PCRE2_UNSET) {
                        listAppend(oneMatch, NIL_VAL);
                    } else {
                        listAppend(oneMatch, (Value){VAL_OBJ, {.obj = (Obj*)copyString(subject + s, (int)(e - s))}});
                    }
                }
                listAppend(allMatches, (Value){VAL_OBJ, {.obj = (Obj*)oneMatch}});
                PCRE2_SIZE matchEnd = ov[1];
                offset = (matchEnd > offset) ? matchEnd : offset + 1;
                pcre2_match_data_free(md);
            }
            pcre2_code_free(re);
            push((Value){VAL_OBJ, {.obj = (Obj*)allMatches}});
            break;
        }

        case BUILTIN_REGEX_REPLACE:
        case BUILTIN_REGEX_REPLACE_ALL: {
            if (argc != 3) {
                return raiseError(id == BUILTIN_REGEX_REPLACE
                    ? "regexReplace() expects 3 arguments (string, pattern, replacement)"
                    : "regexReplaceAll() expects 3 arguments (string, pattern, replacement)");
            }
            Value replVal = pop();
            Value patVal  = pop();
            Value strVal  = pop();
            if (!IS_STRING(strVal)) return raiseError("regex replace: first argument must be a string");
            if (!IS_STRING(patVal)) return raiseError("regex replace: second argument must be a string");
            if (!IS_STRING(replVal)) return raiseError("regex replace: third argument must be a string");

            const char* subject  = AS_STRING(strVal)->chars;
            int         subjLen  = AS_STRING(strVal)->length;
            const char* pattern  = AS_STRING(patVal)->chars;
            const char* repl     = AS_STRING(replVal)->chars;
            int         replLen  = AS_STRING(replVal)->length;
            bool        replAll  = (id == BUILTIN_REGEX_REPLACE_ALL);

            PCRE2_COMPILE(pattern, re);
            uint32_t captureCount = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &captureCount);

            int    resCap = subjLen * 2 + 64;
            char*  resBuf = (char*)malloc((size_t)resCap);
            int    resLen = 0;
            if (!resBuf) { pcre2_code_free(re); return raiseError("regex replace: out of memory"); }

#define RES_APPEND(src, n) do {                                        \
    int _n = (n);                                                       \
    if (resLen + _n >= resCap) {                                       \
        resCap = (resLen + _n + 1) * 2;                                \
        resBuf = (char*)realloc(resBuf, (size_t)resCap);               \
        if (!resBuf) { pcre2_code_free(re);                            \
                       return raiseError("regex replace: out of memory"); } \
    }                                                                   \
    memcpy(resBuf + resLen, (src), (size_t)_n);                        \
    resLen += _n;                                                       \
} while(0)

            PCRE2_SIZE offset = 0;
            bool replaced = false;

            while (offset <= (PCRE2_SIZE)subjLen) {
                if (!replAll && replaced) break;
                pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
                int rc = pcre2_match(re, (PCRE2_SPTR)subject, (PCRE2_SIZE)subjLen,
                                     offset, 0, md, NULL);
                if (rc < 0) {
                    pcre2_match_data_free(md);
                    break;
                }
                PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
                PCRE2_SIZE matchStart = ov[0];
                PCRE2_SIZE matchEnd   = ov[1];

                RES_APPEND(subject + offset, (int)(matchStart - offset));

                for (int ri = 0; ri < replLen; ri++) {
                    if (repl[ri] == '$' && ri + 1 < replLen) {
                        char next = repl[ri + 1];
                        if (next >= '0' && next <= '9') {
                            int groupIdx = next - '0';
                            if ((uint32_t)groupIdx <= captureCount) {
                                PCRE2_SIZE gs = ov[2 * groupIdx];
                                PCRE2_SIZE ge = ov[2 * groupIdx + 1];
                                if (gs != PCRE2_UNSET) {
                                    RES_APPEND(subject + gs, (int)(ge - gs));
                                }
                            }
                            ri++;
                            continue;
                        }
                    }
                    RES_APPEND(repl + ri, 1);
                }

                replaced = true;
                PCRE2_SIZE newOffset = (matchEnd > offset) ? matchEnd : offset + 1;
                if (matchEnd == offset && offset < (PCRE2_SIZE)subjLen) {
                    RES_APPEND(subject + offset, 1);
                }
                offset = newOffset;
                pcre2_match_data_free(md);
            }

            if (offset <= (PCRE2_SIZE)subjLen) {
                RES_APPEND(subject + offset, subjLen - (int)offset);
            }
#undef RES_APPEND

            pcre2_code_free(re);
            ObjString* result = copyString(resBuf, resLen);
            free(resBuf);
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }

        case BUILTIN_REGEX_SPLIT: {
            if (argc != 2) return raiseError("regexSplit() expects 2 arguments (string, pattern)");
            Value patVal = pop();
            Value strVal = pop();
            if (!IS_STRING(strVal)) return raiseError("regexSplit() first argument must be a string");
            if (!IS_STRING(patVal)) return raiseError("regexSplit() second argument must be a string");
            const char* subject = AS_STRING(strVal)->chars;
            int         subjLen = AS_STRING(strVal)->length;
            const char* pattern = AS_STRING(patVal)->chars;

            PCRE2_COMPILE(pattern, re);
            uint32_t captureCount = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &captureCount);

            ObjList* parts = newList();
            PCRE2_SIZE offset = 0;

            while (offset <= (PCRE2_SIZE)subjLen) {
                pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);
                int rc = pcre2_match(re, (PCRE2_SPTR)subject, (PCRE2_SIZE)subjLen,
                                     offset, 0, md, NULL);
                if (rc < 0) {
                    pcre2_match_data_free(md);
                    break;
                }
                PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
                PCRE2_SIZE matchStart = ov[0];
                PCRE2_SIZE matchEnd   = ov[1];

                listAppend(parts, (Value){VAL_OBJ, {.obj = (Obj*)copyString(subject + offset,
                                                      (int)(matchStart - offset))}});
                for (uint32_t i = 1; i <= captureCount; i++) {
                    PCRE2_SIZE gs = ov[2 * i];
                    PCRE2_SIZE ge = ov[2 * i + 1];
                    if (gs == PCRE2_UNSET) {
                        listAppend(parts, NIL_VAL);
                    } else {
                        listAppend(parts, (Value){VAL_OBJ, {.obj = (Obj*)copyString(subject + gs, (int)(ge - gs))}});
                    }
                }
                offset = (matchEnd > offset) ? matchEnd : offset + 1;
                pcre2_match_data_free(md);
            }
            listAppend(parts, (Value){VAL_OBJ, {.obj = (Obj*)copyString(subject + offset,
                                                  subjLen - (int)offset)}});
            pcre2_code_free(re);
            push((Value){VAL_OBJ, {.obj = (Obj*)parts}});
            break;
        }

        default:
            return raiseError("Unknown Regex builtin %d", id);
    }
    return INTERPRET_OK;
}

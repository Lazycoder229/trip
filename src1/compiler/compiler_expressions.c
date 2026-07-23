// compiler_expressions.c — the Pratt (precedence-climbing) expression
// parser: every literal, operator, call form, and the token->parse-rule
// table that drives parsePrecedence().

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "compiler_internal.h"

static void number(void) {
    double value = strtod(parser.previous.start, NULL);
    emitConstant(NUMBER_VAL(value));
}

ObjString* copyStringWithEscapes(const char* raw, int rawLength) {
    char* buf = malloc(rawLength > 0 ? (size_t)rawLength : 1);
    int out = 0;
    for (int i = 0; i < rawLength; i++) {
        char c = raw[i];
        if (c == '\\' && i + 1 < rawLength) {
            char next = raw[++i];
            switch (next) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'r':  buf[out++] = '\r'; break;
                case '0':  buf[out++] = '\0'; break;
                case '\\': buf[out++] = '\\'; break;
                case '"':  buf[out++] = '"';  break;
                case '\'': buf[out++] = '\''; break;
                default:
                    buf[out++] = '\\';
                    buf[out++] = next;
                    break;
            }
        } else {
            buf[out++] = c;
        }
    }
    ObjString* result = copyString(buf, out);
    free(buf);
    return result;
}

static void string(void) {
    ObjString* str = copyStringWithEscapes(parser.previous.start + 1, parser.previous.length - 2);
    Value value = {VAL_OBJ, {.obj = (Obj*)str}};
    emitConstant(value);
}

// ── Multi-line string  """...""" ──────────────────────────────────────────
// Token layout: the raw source span includes the opening and closing """.
// Content is everything between them (6 chars of delimiters total).
//
// Processing steps:
//   1. Strip one leading '\n' (so the opening """ can sit on its own line
//      without adding a blank first line to the value).
//   2. Strip one trailing '\n' before the closing """ (mirror of step 1,
//      so the closing """ can sit on its own line too).
//   3. Compute the minimum indentation of all non-empty lines and remove
//      that many leading spaces/tabs from every line (dedent normalisation,
//      like Python's textwrap.dedent). This lets the string be indented
//      naturally with the surrounding code without that indent leaking
//      into the value.
//   4. Process escape sequences with copyStringWithEscapes().
static void multilineString(void) {
    // Content sits between the opening """ and closing """.
    const char* raw    = parser.previous.start + 3;
    int         rawLen = parser.previous.length - 6;
    if (rawLen < 0) rawLen = 0;

    // Step 1: strip one leading newline.
    if (rawLen > 0 && raw[0] == '\n') { raw++; rawLen--; }
    else if (rawLen > 1 && raw[0] == '\r' && raw[1] == '\n') { raw += 2; rawLen -= 2; }

    // Step 2: strip one trailing newline (before the closing """).
    if (rawLen > 0 && raw[rawLen - 1] == '\n') {
        rawLen--;
        if (rawLen > 0 && raw[rawLen - 1] == '\r') rawLen--;
    }

    // Step 3: compute minimum indentation of non-empty lines.
    int minIndent = INT_MAX;
    int col = 0;
    bool lineHasContent = false;
    for (int i = 0; i <= rawLen; i++) {
        char ch = (i < rawLen) ? raw[i] : '\n'; // treat end-of-content as newline
        if (ch == '\n') {
            if (lineHasContent && col < minIndent) minIndent = col;
            col = 0;
            lineHasContent = false;
        } else if (ch == ' ' || ch == '\t') {
            if (!lineHasContent) col++;
        } else {
            lineHasContent = true;
        }
    }
    if (minIndent == INT_MAX) minIndent = 0; // all lines empty

    // Build the dedented content into a temporary buffer, then pass through
    // copyStringWithEscapes() for escape processing.
    char* buf = malloc((size_t)rawLen + 1);
    int out = 0;
    col = 0;
    for (int i = 0; i < rawLen; i++) {
        char ch = raw[i];
        if (ch == '\n') {
            buf[out++] = '\n';
            col = 0;
        } else if (col < minIndent && (ch == ' ' || ch == '\t')) {
            // Still inside the stripped indentation prefix — skip this char.
            col++;
        } else {
            buf[out++] = ch;
            col = minIndent + 1; // past the prefix, stop checking
        }
    }
    buf[out] = '\0';

    ObjString* str = copyStringWithEscapes(buf, out);
    free(buf);
    Value value = {VAL_OBJ, {.obj = (Obj*)str}};
    emitConstant(value);
}

// ── Raw string  r"..." ────────────────────────────────────────────────────
// Content sits between the opening r" and closing ".
// No escape processing — backslashes are literal characters.
static void rawString(void) {
    // Token: r"..." — skip 'r' and opening '"', skip closing '"'.
    const char* raw    = parser.previous.start + 2; // past r and "
    int         rawLen = parser.previous.length - 3; // minus r, ", closing "
    if (rawLen < 0) rawLen = 0;

    ObjString* str = copyString(raw, rawLen); // copyString, NOT copyStringWithEscapes
    Value value = {VAL_OBJ, {.obj = (Obj*)str}};
    emitConstant(value);
}

// ── f-string expression ───────────────────────────────────────────────────
// Desugars f"literal {expr} more" into a chain of string concatenations.
// Each {expr} is compiled by temporarily re-pointing the scanner at just
// that sub-string, then restoring all parser/scanner state (including
// hadError/panicMode) so any error inside one interpolation doesn't
// poison the rest of the outer parse.
static void fstringExpr(void) {
    const char* content = parser.previous.start + 2;   // skip f"
    int contentLen = parser.previous.length - 3;        // minus f, ", closing "

    int segStart = 0;
    bool emittedAny = false;

    int i = 0;
    while (i < contentLen) {
        if (content[i] == '\\' && i + 1 < contentLen) {
            i += 2;
            continue;
        }
        if (content[i] == '{') {
            // Flush literal run so far
            int litLen = i - segStart;
            if (litLen > 0 || !emittedAny) {
                ObjString* lit = copyStringWithEscapes(content + segStart, litLen);
                emitConstant((Value){VAL_OBJ, {.obj = (Obj*)lit}});
                if (emittedAny) emitByte(OP_ADD);
                emittedAny = true;
            }

            // Find matching '}'
            int exprStart = i + 1;
            int j = exprStart;
            int depth = 1;
            while (j < contentLen && depth > 0) {
                // Skip over string literals so '}' / '{' inside them
                // don't mess up the brace depth count.
                // A string opens on an unescaped " (i.e. not preceded by \).
                if (content[j] == '"' &&
                    (j == 0 || content[j - 1] != '\\')) {
                    j++; // skip opening quote
                    while (j < contentLen) {
                        if (content[j] == '\\' && j + 1 < contentLen) {
                            j += 2; // skip escape sequence (e.g. \")
                            continue;
                        }
                        if (content[j] == '"') break; // closing quote
                        j++;
                    }
                    if (j < contentLen) j++; // skip closing quote
                    continue;
                }
                if (content[j] == '{') depth++;
                else if (content[j] == '}') { depth--; if (depth == 0) break; }
                j++;
            }
            if (depth != 0) {
                errorAt(&parser.previous, "Unterminated '{' in f-string.");
                return;
            }
            int exprLen = j - exprStart;

            // Copy the expression, unescaping \" -> " so the sub-scanner
            // sees plain string literals (it won't have the outer f-string
            // context, so backslash-escaped quotes must become real quotes).
            char* exprCopy = malloc((size_t)exprLen + 1);
            int outLen = 0;
            for (int k = 0; k < exprLen; k++) {
                if (content[exprStart + k] == '\\' &&
                    k + 1 < exprLen &&
                    content[exprStart + k + 1] == '"') {
                    exprCopy[outLen++] = '"';
                    k++; // skip the backslash, loop will skip the quote
                } else {
                    exprCopy[outLen++] = content[exprStart + k];
                }
            }
            exprCopy[outLen] = '\0';

            // ── Save ALL parser + scanner state ──────────────────────────
            Scanner savedScanner   = scanner;
            Token   savedCurrent   = parser.current;
            Token   savedPrevious  = parser.previous;
            bool    savedHadError  = parser.hadError;
            bool    savedPanicMode = parser.panicMode;

            // Reset error state so the sub-expression gets a clean parse
            parser.hadError  = false;
            parser.panicMode = false;

            initScanner(exprCopy);
            advance();
            expression();

            bool exprError = parser.hadError;
            if (!exprError && parser.current.type != TOKEN_EOF) {
                errorAt(&parser.current, "Unexpected token in f-string expression.");
                exprError = true;
            }

            // ── Restore ALL parser + scanner state ───────────────────────
            scanner          = savedScanner;
            parser.current   = savedCurrent;
            parser.previous  = savedPrevious;
            // Merge: keep any outer error; OR in the sub-expression error
            parser.hadError  = savedHadError || exprError;
            // Restore outer panicMode — don't let sub-scan panic bleed out
            parser.panicMode = savedPanicMode;

            free(exprCopy);

            // Convert the expression result to string, then concatenate
            emitByte(OP_CALL_BUILTIN);
            emitByte(BUILTIN_STR);
            emitByte(1);
            if (emittedAny) emitByte(OP_ADD);
            emittedAny = true;

            segStart = j + 1;
            i = j + 1;
            continue;
        }
        i++;
    }

    // Flush trailing literal run (or emit empty string if needed)
    int litLen = contentLen - segStart;
    if (litLen > 0 || !emittedAny) {
        ObjString* lit = copyStringWithEscapes(content + segStart, litLen);
        emitConstant((Value){VAL_OBJ, {.obj = (Obj*)lit}});
        if (emittedAny) emitByte(OP_ADD);
        emittedAny = true;
    }
    (void)emittedAny;
}

static void charLiteral(void) {
    const char* p = parser.previous.start + 1;
    char ch;
    if (*p == '\\') {
        char next = p[1];
        switch (next) {
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case 'r':  ch = '\r'; break;
            case '0':  ch = '\0'; break;
            case '\\': ch = '\\'; break;
            case '\'': ch = '\''; break;
            case '"':  ch = '"';  break;
            default:   ch = next; break;
        }
    } else {
        ch = *p;
    }
    emitConstant(CHAR_VAL(ch));
}

// ── List literal  [a, b, c] ──────────────────────────────────────────────
static void listLiteral(void) {
    uint8_t count = 0;
    while (parser.current.type != TOKEN_RIGHT_BRACKET && parser.current.type != TOKEN_EOF) {
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_RIGHT_BRACKET) break;
        expression();
        count++;
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_COMMA) advance();
    }
    consume(TOKEN_RIGHT_BRACKET, "Expected ']' after list items.");
    emitByte(OP_BUILD_LIST);
    emitByte(count);
}

// ── Dict literal  {"key": val, ...} ──────────────────────────────────────
static void dictLiteral(void) {
    uint8_t pairs = 0;
    while (parser.current.type != TOKEN_RIGHT_BRACE && parser.current.type != TOKEN_EOF) {
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_RIGHT_BRACE) break;
        expression();
        consume(TOKEN_COLON, "Expected ':' after dict key.");
        expression();
        pairs++;
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_COMMA) advance();
    }
    consume(TOKEN_RIGHT_BRACE, "Expected '}' after dict entries.");
    emitByte(OP_BUILD_DICT);
    emitByte(pairs);
}

// ── Pratt parser types ────────────────────────────────────────────────────
typedef void (*ParseFn)(void);
typedef struct { ParseFn prefix; ParseFn infix; Precedence precedence; } ParseRule;
static ParseRule* getRule(TokenKind type);

static void literal(void) {
    switch (parser.previous.type) {
        case TOKEN_TRUE:  emitConstant(BOOL_VAL(true));  break;
        case TOKEN_FALSE: emitConstant(BOOL_VAL(false)); break;
        case TOKEN_NIL:   emitByte(OP_NIL);              break;
        default: return;
    }
}

static void binary(void) {
    TokenKind operatorType = parser.previous.type;
    ParseRule* rule = getRule(operatorType);
    parsePrecedence((Precedence)(rule->precedence + 1));

    switch (operatorType) {
        case TOKEN_PLUS:          emitByte(OP_ADD);          break;
        case TOKEN_MINUS:         emitByte(OP_SUBTRACT);     break;
        case TOKEN_STAR:          emitByte(OP_MULTIPLY);     break;
        case TOKEN_SLASH:         emitByte(OP_DIVIDE);       break;
        case TOKEN_PERCENT:       emitByte(OP_MODULO);       break;
        case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL);        break;
        case TOKEN_BANG_EQUAL:    emitByte(OP_NOT_EQUAL);    break;
        case TOKEN_LESS:          emitByte(OP_LESS);         break;
        case TOKEN_LESS_EQUAL:    emitByte(OP_LESS_EQUAL);   break;
        case TOKEN_GREATER:       emitByte(OP_GREATER);      break;
        case TOKEN_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL);break;
        default: return;
    }
}

// ── Built-in resolver ─────────────────────────────────────────────────────
static int resolveBuiltin(const char* name, int len) {
    struct { const char* nm; int id; } tbl[] = {
        {"len",   BUILTIN_LEN},   {"type",  BUILTIN_TYPE},
        {"int",   BUILTIN_INT},   {"float", BUILTIN_FLOAT},
        {"str",   BUILTIN_STR},   {"char",  BUILTIN_CHAR},
        {"ord",   BUILTIN_ORD},   {"input", BUILTIN_INPUT},
        {"range", BUILTIN_RANGE},
        {"readFile",   BUILTIN_FILE_READ},
        {"writeFile",  BUILTIN_FILE_WRITE},
        {"appendFile", BUILTIN_FILE_APPEND},
        {"fileExists", BUILTIN_FILE_EXISTS},
        {"deleteFile", BUILTIN_FILE_DELETE},
        {"args",       BUILTIN_ARGS},
        {"getEnv",     BUILTIN_GETENV},
        {"scriptPath", BUILTIN_SCRIPT_PATH},
        {"sqrt",       BUILTIN_SQRT},
        {"pow",        BUILTIN_POW},
        {"abs",        BUILTIN_ABS},
        {"min",        BUILTIN_MIN},
        {"max",        BUILTIN_MAX},
        {"floor",      BUILTIN_FLOOR},
        {"ceil",       BUILTIN_CEIL},
        {"round",      BUILTIN_ROUND},
        {"random",        BUILTIN_RANDOM},
        {"randomInt",     BUILTIN_RANDOM_INT},
        {"jsonParse",     BUILTIN_JSON_PARSE},
        {"jsonStringify", BUILTIN_JSON_STRINGIFY},
        {"httpGet",    BUILTIN_HTTP_GET},
        {"httpPost",   BUILTIN_HTTP_POST},
        {"httpPut",    BUILTIN_HTTP_PUT},
        {"httpDelete", BUILTIN_HTTP_DELETE},
        {"httpPatch",  BUILTIN_HTTP_PATCH},
        {"tcpListen",  BUILTIN_TCP_LISTEN},
        {"tcpAccept",  BUILTIN_TCP_ACCEPT},
        {"tcpConnect", BUILTIN_TCP_CONNECT},
        {"tcpRead",    BUILTIN_TCP_READ},
        {"tcpWrite",   BUILTIN_TCP_WRITE},
        {"tcpClose",   BUILTIN_TCP_CLOSE},
        {"tlsListen",  BUILTIN_TLS_LISTEN},
        {"tlsAccept",  BUILTIN_TLS_ACCEPT},
        {"tlsConnect", BUILTIN_TLS_CONNECT},
        {"tlsRead",    BUILTIN_TLS_READ},
        {"tlsWrite",   BUILTIN_TLS_WRITE},
        {"tlsClose",   BUILTIN_TLS_CLOSE},
        {"spawn",        BUILTIN_SPAWN},  // kept for spawn() as expression fallback
        {"yield",        BUILTIN_YIELD},
        {"waitAll",      BUILTIN_WAIT_ALL},
        {"httpParse",    BUILTIN_HTTP_PARSE},
        {"httpResponse", BUILTIN_HTTP_RESPONSE},
        {"wsHandshake",  BUILTIN_WS_HANDSHAKE},
        {"wsRead",       BUILTIN_WS_READ},
        {"wsWrite",      BUILTIN_WS_WRITE},
        {"wsClose",      BUILTIN_WS_CLOSE},
        {"httpChunkedStart", BUILTIN_HTTP_CHUNKED_START},
        {"httpChunkWrite",   BUILTIN_HTTP_CHUNK_WRITE},
        {"httpChunkEnd",     BUILTIN_HTTP_CHUNK_END},
        {"sseWrite",         BUILTIN_SSE_WRITE},
        // Date/Time
        {"time",       BUILTIN_TIME},
        {"timeMs",     BUILTIN_TIME_MS},
        {"sleep",      BUILTIN_SLEEP},
        // Date/Calendar
        {"dateNow",    BUILTIN_DATE_NOW},
        {"dateUTC",    BUILTIN_DATE_UTC},
        {"dateLocal",  BUILTIN_DATE_LOCAL},
        {"dateFormat", BUILTIN_DATE_FORMAT},
        {"dateParse",  BUILTIN_DATE_PARSE},
        {"dateMake",   BUILTIN_DATE_MAKE},
        // Regex (PCRE2)
        {"regexMatch",      BUILTIN_REGEX_MATCH},
        {"regexMatchAll",   BUILTIN_REGEX_MATCH_ALL},
        {"regexTest",       BUILTIN_REGEX_TEST},
        {"regexReplace",    BUILTIN_REGEX_REPLACE},
        {"regexReplaceAll", BUILTIN_REGEX_REPLACE_ALL},
        {"regexSplit",      BUILTIN_REGEX_SPLIT},
    };
    for (int i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++)
        if ((int)strlen(tbl[i].nm) == len && memcmp(tbl[i].nm, name, len) == 0)
            return tbl[i].id;
    return -1;
}

static int resolveMethod(const char* name, int len) {
    struct { const char* nm; int id; } tbl[] = {
        {"upper",      METHOD_STR_UPPER},
        {"lower",      METHOD_STR_LOWER},
        {"trim",       METHOD_STR_TRIM},
        {"split",      METHOD_STR_SPLIT},
        {"startsWith", METHOD_STR_STARTS_WITH},
        {"endsWith",   METHOD_STR_ENDS_WITH},
        {"replace",    METHOD_STR_REPLACE},
        {"find",       METHOD_STR_FIND},
        {"repeat",     METHOD_STR_REPEAT},
        {"len",        METHOD_LIST_LEN},
        {"contains",   METHOD_LIST_CONTAINS},
        {"slice",      METHOD_LIST_SLICE},
        {"append",     METHOD_LIST_APPEND},
        {"push",       METHOD_LIST_PUSH},
        {"pop",        METHOD_LIST_POP},
        {"insert",     METHOD_LIST_INSERT},
        {"remove",     METHOD_LIST_REMOVE},
        {"reverse",    METHOD_LIST_REVERSE},
        {"sort",       METHOD_LIST_SORT},
        {"join",       METHOD_LIST_JOIN},
        {"clear",      METHOD_LIST_CLEAR},
        {"map",        METHOD_LIST_MAP},
        {"filter",     METHOD_LIST_FILTER},
        {"reduce",     METHOD_LIST_REDUCE},
        {"forEach",    METHOD_LIST_FOR_EACH},
        {"indexOf",    METHOD_LIST_INDEX_OF},
        {"flatten",    METHOD_LIST_FLATTEN},
        {"zip",        METHOD_LIST_ZIP},
        {"enumerate",  METHOD_LIST_ENUMERATE},
        {"chars",      METHOD_STR_CHARS},
        {"format",     METHOD_STR_FORMAT},
        {"padLeft",    METHOD_STR_PAD_LEFT},
        {"padRight",   METHOD_STR_PAD_RIGHT},
        {"isDigit",    METHOD_STR_IS_DIGIT},
        {"isAlpha",    METHOD_STR_IS_ALPHA},
        {"isUpper",    METHOD_STR_IS_UPPER},
        {"isLower",    METHOD_STR_IS_LOWER},
        {"reverse",    METHOD_STR_REVERSE},
        {"get",        METHOD_DICT_GET},
        {"set",        METHOD_DICT_SET},
        {"del",        METHOD_DICT_DEL},
        {"keys",       METHOD_DICT_KEYS},
        {"values",     METHOD_DICT_VALUES},
        {"has",        METHOD_DICT_HAS},
    };
    for (int i = 0; i < (int)(sizeof(tbl)/sizeof(tbl[0])); i++)
        if ((int)strlen(tbl[i].nm) == len && memcmp(tbl[i].nm, name, len) == 0)
            return tbl[i].id;
    return -1;
}

// ── Argument list parser helper ───────────────────────────────────────────
// Shared by variable(), dotExpr(), and subscriptExpr() for parsing (arg, arg, ...)
// Caller has already consumed the '('. Returns argc.
// Skips NEWLINE/INDENT/DEDENT between arguments to support multiline calls.
static uint8_t parseArgList(void) {
    uint8_t argc = 0;
    while (parser.current.type != TOKEN_RIGHT_PAREN && parser.current.type != TOKEN_EOF) {
        // Skip whitespace/indent tokens that appear in multiline argument lists
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_RIGHT_PAREN) break;
        expression();
        argc++;
        while (parser.current.type == TOKEN_NEWLINE ||
               parser.current.type == TOKEN_INDENT  ||
               parser.current.type == TOKEN_DEDENT) advance();
        if (parser.current.type == TOKEN_COMMA) advance();
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return argc;
}

static void dotExpr(void) {
    consume(TOKEN_IDENTIFIER, "Expected property name after '.'.");
    Token name = parser.previous;
    uint16_t nameConstant = identifierConstant(&name);

    if (match(TOKEN_EQUAL)) {
        expression();
        emitByte(OP_PROPERTY_SET);
        emitShort(nameConstant);
    } else if (parser.current.type == TOKEN_LEFT_PAREN) {
        advance();
        uint8_t argc = parseArgList();
        int methodId = resolveMethod(name.start, name.length);
        if (methodId >= 0) {
            emitByte(OP_CALL_METHOD);
            emitByte((uint8_t)methodId);
            emitByte(argc);
        } else {
            emitByte(OP_INVOKE);
            emitShort(nameConstant);
            emitByte(argc);
        }
    } else {
        emitByte(OP_PROPERTY_GET);
        emitShort(nameConstant);
    }
}

// ── Subscript infix:  a[i] or a[i] = v ──────────────────────────────────
static void subscriptExpr(void) {
    expression();
    consume(TOKEN_RIGHT_BRACKET, "Expected ']' after index.");

    if (match(TOKEN_EQUAL)) {
        expression();
        emitByte(OP_INDEX_SET);
    } else {
        emitByte(OP_INDEX_GET);
        if (parser.current.type == TOKEN_LEFT_PAREN) {
            advance();
            uint8_t argc = parseArgList();
            emitByte(OP_CALL);
            emitByte(argc);
        }
    }
}

static void grouping(void) {
    expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void andExpr(void) {
    int skipRight = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    parsePrecedence(PREC_AND);
    patchJump(skipRight);
}

static void orExpr(void) {
    int skipToRight = emitJump(OP_JUMP_IF_FALSE);
    int skipRight   = emitJump(OP_JUMP);
    patchJump(skipToRight);
    emitByte(OP_POP);
    parsePrecedence(PREC_OR);
    patchJump(skipRight);
}

static void unary(void) {
    TokenKind operatorType = parser.previous.type;
    parsePrecedence(PREC_UNARY);
    switch (operatorType) {
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        case TOKEN_BANG:  emitByte(OP_NOT);    break;
        case TOKEN_NOT:   emitByte(OP_NOT);    break;
        default: return;
    }
}

static void typeofExpr(void) {
    parsePrecedence(PREC_UNARY);   // parse the operand: typeof x, typeof (a+b), etc.
    emitByte(OP_CALL_BUILTIN);
    emitByte(BUILTIN_TYPE);
    emitByte(1);   // argc = 1
}

bool identifierEqual(const Token* token, const char* text) {
    return token->length == (int)strlen(text) && memcmp(token->start, text, token->length) == 0;
}

static void compoundAssign(Token* name, uint8_t op) {
    uint16_t nameConstant = identifierConstant(name);
    emitByte(OP_GET_GLOBAL);
    emitShort(nameConstant);
    expression();
    emitByte(op);
    emitByte(OP_SET_GLOBAL);
    emitShort(nameConstant);
}

static void incrementOrDecrement(Token* name, uint8_t op) {
    uint16_t nameConstant = identifierConstant(name);
    emitByte(OP_GET_GLOBAL);
    emitShort(nameConstant);
    emitConstant(NUMBER_VAL(1));
    emitByte(op);
    emitByte(OP_SET_GLOBAL);
    emitShort(nameConstant);
}

static void compoundAssignLocal(int slot, uint8_t op) {
    emitByte(OP_GET_LOCAL);
    emitByte((uint8_t)slot);
    expression();
    emitByte(op);
    emitByte(OP_SET_LOCAL);
    emitByte((uint8_t)slot);
}

static void incrementOrDecrementLocal(int slot, uint8_t op) {
    emitByte(OP_GET_LOCAL);
    emitByte((uint8_t)slot);
    emitConstant(NUMBER_VAL(1));
    emitByte(op);
    emitByte(OP_SET_LOCAL);
    emitByte((uint8_t)slot);
}

static void compoundAssignUpvalue(int slot, uint8_t op) {
    emitByte(OP_GET_UPVALUE);
    emitByte((uint8_t)slot);
    expression();
    emitByte(op);
    emitByte(OP_SET_UPVALUE);
    emitByte((uint8_t)slot);
}

static void incrementOrDecrementUpvalue(int slot, uint8_t op) {
    emitByte(OP_GET_UPVALUE);
    emitByte((uint8_t)slot);
    emitConstant(NUMBER_VAL(1));
    emitByte(op);
    emitByte(OP_SET_UPVALUE);
    emitByte((uint8_t)slot);
}

static void variable(void) {
    Token name = parser.previous;

    if (identifierEqual(&name, "out") || identifierEqual(&name, "outn")) {
        if (match(TOKEN_LEFT_PAREN)) {
            expression();
            consume(TOKEN_RIGHT_PAREN, "Expect ')' after argument.");
            emitByte(identifierEqual(&name, "outn") ? OP_PRINT_NL : OP_PRINT);
            lastExprWasVoid = true;
            return;
        }
    }

    // ── Built-in function call ────────────────────────────────────────────
    {
        int bid = resolveBuiltin(name.start, name.length);
        if (bid >= 0 && parser.current.type == TOKEN_LEFT_PAREN) {
            advance();
            uint8_t argc = parseArgList();
            emitByte(OP_CALL_BUILTIN);
            emitByte((uint8_t)bid);
            emitByte(argc);
            return;
        }
    }

    int localSlot = resolveLocal(&name);
    int upvalSlot = (localSlot < 0) ? resolveUpvalue(&name) : -1;

    if (localSlot >= 0) {
        if (parser.current.type == TOKEN_LEFT_PAREN) {
            advance();
            emitByte(OP_GET_LOCAL);
            emitByte((uint8_t)localSlot);
            uint8_t argc = parseArgList();
            emitByte(OP_CALL);
            emitByte(argc);
            return;
        }
        if (match(TOKEN_EQUAL)) {
            if (current->locals[localSlot].isConst) {
                errorAt(&parser.previous, "Cannot reassign a const variable.");
            }
            expression();
            emitByte(OP_SET_LOCAL);
            emitByte((uint8_t)localSlot);
        } else if (match(TOKEN_PLUS_EQUAL)) {
            compoundAssignLocal(localSlot, OP_ADD);
        } else if (match(TOKEN_MINUS_EQUAL)) {
            compoundAssignLocal(localSlot, OP_SUBTRACT);
        } else if (match(TOKEN_STAR_EQUAL)) {
            compoundAssignLocal(localSlot, OP_MULTIPLY);
        } else if (match(TOKEN_SLASH_EQUAL)) {
            compoundAssignLocal(localSlot, OP_DIVIDE);
        } else if (match(TOKEN_PLUS_PLUS)) {
            incrementOrDecrementLocal(localSlot, OP_ADD);
        } else if (match(TOKEN_MINUS_MINUS)) {
            incrementOrDecrementLocal(localSlot, OP_SUBTRACT);
        } else {
            emitByte(OP_GET_LOCAL);
            emitByte((uint8_t)localSlot);
        }
    } else if (upvalSlot >= 0) {
        if (parser.current.type == TOKEN_LEFT_PAREN) {
            advance();
            emitByte(OP_GET_UPVALUE);
            emitByte((uint8_t)upvalSlot);
            uint8_t argc = parseArgList();
            emitByte(OP_CALL);
            emitByte(argc);
            return;
        }
        if (match(TOKEN_EQUAL)) {
            if (current->upvalues[upvalSlot].isConst) {
                errorAt(&parser.previous, "Cannot reassign a const variable.");
            }
            expression();
            emitByte(OP_SET_UPVALUE);
            emitByte((uint8_t)upvalSlot);
        } else if (match(TOKEN_PLUS_EQUAL)) {
            compoundAssignUpvalue(upvalSlot, OP_ADD);
        } else if (match(TOKEN_MINUS_EQUAL)) {
            compoundAssignUpvalue(upvalSlot, OP_SUBTRACT);
        } else if (match(TOKEN_STAR_EQUAL)) {
            compoundAssignUpvalue(upvalSlot, OP_MULTIPLY);
        } else if (match(TOKEN_SLASH_EQUAL)) {
            compoundAssignUpvalue(upvalSlot, OP_DIVIDE);
        } else if (match(TOKEN_PLUS_PLUS)) {
            incrementOrDecrementUpvalue(upvalSlot, OP_ADD);
        } else if (match(TOKEN_MINUS_MINUS)) {
            incrementOrDecrementUpvalue(upvalSlot, OP_SUBTRACT);
        } else {
            emitByte(OP_GET_UPVALUE);
            emitByte((uint8_t)upvalSlot);
        }
    } else {
        if (parser.current.type == TOKEN_LEFT_PAREN) {
            advance();
            emitByte(OP_GET_GLOBAL);
            emitShort(identifierConstant(&name));
            uint8_t argc = parseArgList();
            emitByte(OP_CALL);
            emitByte(argc);
            return;
        }
        if (match(TOKEN_EQUAL)) {
            expression();
            emitByte(OP_SET_GLOBAL);
            emitShort(identifierConstant(&name));
        } else if (match(TOKEN_PLUS_EQUAL)) {
            compoundAssign(&name, OP_ADD);
        } else if (match(TOKEN_MINUS_EQUAL)) {
            compoundAssign(&name, OP_SUBTRACT);
        } else if (match(TOKEN_STAR_EQUAL)) {
            compoundAssign(&name, OP_MULTIPLY);
        } else if (match(TOKEN_SLASH_EQUAL)) {
            compoundAssign(&name, OP_DIVIDE);
        } else if (match(TOKEN_PLUS_PLUS)) {
            incrementOrDecrement(&name, OP_ADD);
        } else if (match(TOKEN_MINUS_MINUS)) {
            incrementOrDecrement(&name, OP_SUBTRACT);
        } else {
            emitByte(OP_GET_GLOBAL);
            emitShort(identifierConstant(&name));
        }
    }
}

static ParseRule rules[] = {
    [TOKEN_LEFT_PAREN]     = {grouping,    NULL,          PREC_NONE},
    [TOKEN_RIGHT_PAREN]    = {NULL,        NULL,          PREC_NONE},
    [TOKEN_PLUS]           = {NULL,        binary,        PREC_TERM},
    [TOKEN_MINUS]          = {unary,       binary,        PREC_TERM},
    [TOKEN_STAR]           = {NULL,        binary,        PREC_FACTOR},
    [TOKEN_SLASH]          = {NULL,        binary,        PREC_FACTOR},
    [TOKEN_EQUAL_EQUAL]    = {NULL,        binary,        PREC_EQUALITY},
    [TOKEN_BANG_EQUAL]     = {NULL,        binary,        PREC_EQUALITY},
    [TOKEN_LESS]           = {NULL,        binary,        PREC_COMPARISON},
    [TOKEN_LESS_EQUAL]     = {NULL,        binary,        PREC_COMPARISON},
    [TOKEN_GREATER]        = {NULL,        binary,        PREC_COMPARISON},
    [TOKEN_GREATER_EQUAL]  = {NULL,        binary,        PREC_COMPARISON},
    [TOKEN_BANG]           = {unary,       NULL,          PREC_NONE},
    [TOKEN_NOT]            = {unary,       NULL,          PREC_NONE},
    [TOKEN_TYPEOF]          = {typeofExpr,  NULL,          PREC_NONE},
    [TOKEN_PLUS_EQUAL]     = {NULL,        NULL,          PREC_NONE},
    [TOKEN_MINUS_EQUAL]    = {NULL,        NULL,          PREC_NONE},
    [TOKEN_STAR_EQUAL]     = {NULL,        NULL,          PREC_NONE},
    [TOKEN_SLASH_EQUAL]    = {NULL,        NULL,          PREC_NONE},
    [TOKEN_PLUS_PLUS]      = {NULL,        NULL,          PREC_NONE},
    [TOKEN_MINUS_MINUS]    = {NULL,        NULL,          PREC_NONE},
    [TOKEN_IDENTIFIER]     = {variable,    NULL,          PREC_NONE},
    [TOKEN_SELF]           = {variable,    NULL,          PREC_NONE},
    [TOKEN_STRING]         = {string,           NULL,          PREC_NONE},
    [TOKEN_FSTRING]        = {fstringExpr,       NULL,          PREC_NONE},
    [TOKEN_MULTILINE_STRING] = {multilineString, NULL,          PREC_NONE},
    [TOKEN_RAW_STRING]     = {rawString,         NULL,          PREC_NONE},
    [TOKEN_NUMBER]         = {number,      NULL,          PREC_NONE},
    [TOKEN_CHAR_LIT]       = {charLiteral, NULL,          PREC_NONE},
    [TOKEN_TRUE]           = {literal,     NULL,          PREC_NONE},
    [TOKEN_FALSE]          = {literal,     NULL,          PREC_NONE},
    [TOKEN_NIL]            = {literal,     NULL,          PREC_NONE},
    [TOKEN_LEFT_BRACKET]   = {listLiteral, subscriptExpr, PREC_PRIMARY},
    [TOKEN_LEFT_BRACE]     = {dictLiteral, NULL,          PREC_NONE},
    [TOKEN_DOT]            = {NULL,        dotExpr,       PREC_PRIMARY},
    [TOKEN_PERCENT]        = {NULL,        binary,        PREC_FACTOR},
    [TOKEN_COMMA]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_COLON]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_RIGHT_BRACKET]  = {NULL,        NULL,          PREC_NONE},
    [TOKEN_RIGHT_BRACE]    = {NULL,        NULL,          PREC_NONE},
    [TOKEN_LET]            = {NULL,        NULL,          PREC_NONE},
    [TOKEN_CONST]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_IF]             = {NULL,        NULL,          PREC_NONE},
    [TOKEN_ELSE]           = {NULL,        NULL,          PREC_NONE},
    [TOKEN_WHILE]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_BREAK]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_CONTINUE]       = {NULL,        NULL,          PREC_NONE},
    [TOKEN_TRY]            = {NULL,        NULL,          PREC_NONE},
    [TOKEN_CATCH]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_THROW]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_FINALLY]        = {NULL,        NULL,          PREC_NONE},
    [TOKEN_IMPORT]         = {NULL,        NULL,          PREC_NONE},
    [TOKEN_AND]            = {NULL,        andExpr,       PREC_AND},
    [TOKEN_OR]             = {NULL,        orExpr,        PREC_OR},
    [TOKEN_ELIF]           = {NULL,        NULL,          PREC_NONE},
    [TOKEN_FOR]            = {NULL,        NULL,          PREC_NONE},
    [TOKEN_IN]             = {NULL,        NULL,          PREC_NONE},
    [TOKEN_DOTDOT]         = {NULL,        NULL,          PREC_NONE},
    [TOKEN_FN]             = {lambdaExpr,  NULL,          PREC_NONE},
    [TOKEN_RETURN]         = {NULL,        NULL,          PREC_NONE},
    [TOKEN_NEWLINE]        = {NULL,        NULL,          PREC_NONE},
    [TOKEN_INDENT]         = {NULL,        NULL,          PREC_NONE},
    [TOKEN_DEDENT]         = {NULL,        NULL,          PREC_NONE},
    [TOKEN_MATCH]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_CASE]           = {NULL,        NULL,          PREC_NONE},
    [TOKEN_ASYNC]          = {asyncLambdaExpr, NULL,      PREC_NONE},
    [TOKEN_AWAIT]          = {awaitExpr,   NULL,          PREC_NONE},
    [TOKEN_ERROR]          = {NULL,        NULL,          PREC_NONE},
    [TOKEN_EOF]            = {NULL,        NULL,          PREC_NONE},
};

static ParseRule* getRule(TokenKind type) {
    return &rules[type];
}

void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        errorAt(&parser.previous, "Expect expression.");
        return;
    }
    prefixRule();

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule();
    }
}

void expression(void) {
    parsePrecedence(PREC_ASSIGNMENT);
}
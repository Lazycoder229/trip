// compiler_functions.c — named and anonymous function compilation:
// parameter-list/default-value parsing, `fn` declarations, lambda
// expressions, and class/method declarations.

#include <stdlib.h>
#include "compiler_internal.h"

// ── Default parameter helpers ─────────────────────────────────────────────

// Attempt to parse one constant-expression default value (the part after '=').
// Only literal tokens are accepted — numbers, strings, booleans, nil, chars.
// Returns true and fills *out on success; emits an error and returns false
// if the expression is not a compile-time constant.
static bool parseConstantDefault(Value* out) {
    // Peek at the current token (not yet consumed).
    switch (parser.current.type) {
        case TOKEN_NUMBER: {
            advance();
            *out = NUMBER_VAL(strtod(parser.previous.start, NULL));
            return true;
        }
        case TOKEN_TRUE: {
            advance();
            *out = BOOL_VAL(true);
            return true;
        }
        case TOKEN_FALSE: {
            advance();
            *out = BOOL_VAL(false);
            return true;
        }
        case TOKEN_NIL: {
            advance();
            *out = NIL_VAL;
            return true;
        }
        case TOKEN_STRING: {
            advance();
            ObjString* s = copyStringWithEscapes(
                parser.previous.start + 1, parser.previous.length - 2);
            *out = (Value){VAL_OBJ, {.obj = (Obj*)s}};
            return true;
        }
        case TOKEN_CHAR_LIT: {
            advance();
            const char* p = parser.previous.start + 1;
            char ch;
            if (*p == '\\') {
                switch (p[1]) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case 'r':  ch = '\r'; break;
                    case '0':  ch = '\0'; break;
                    case '\\': ch = '\\'; break;
                    case '\'': ch = '\''; break;
                    case '"':  ch = '"';  break;
                    default:   ch = p[1]; break;
                }
            } else {
                ch = *p;
            }
            *out = CHAR_VAL(ch);
            return true;
        }
        case TOKEN_MINUS: {
            // Allow negative number literals:  fn f(x = -1)
            advance();
            if (parser.current.type == TOKEN_NUMBER) {
                advance();
                *out = NUMBER_VAL(-strtod(parser.previous.start, NULL));
                return true;
            }
            errorAt(&parser.current,
                "Default parameter value must be a constant (number, string, bool, nil, or char).");
            return false;
        }
        default:
            errorAt(&parser.current,
                "Default parameter value must be a constant (number, string, bool, nil, or char).");
            return false;
    }
}

// Parse a parameter list into `params[]` (max 255 entries).
// Fills hasDefault/defaultVal for parameters that have '= <constant>'.
// All defaulted parameters must come after all required ones.
// Returns the number of parameters parsed.
int parseParamList(ParamInfo params[256]) {
    int count = 0;
    bool seenDefault  = false;
    bool seenVariadic = false;

    if (parser.current.type != TOKEN_RIGHT_PAREN) {
        do {
            if (count >= 255) {
                errorAt(&parser.current, "Too many parameters.");
                return count;
            }
            if (seenVariadic) {
                errorAt(&parser.current, "Variadic parameter must be the last parameter.");
                return count;
            }

            bool isVariadic = match(TOKEN_ELLIPSIS);   // consumes '...' if present

            if (parser.current.type != TOKEN_IDENTIFIER) {
                errorAt(&parser.current, "Expect parameter name.");
                return count;
            }
            advance();
            params[count].name       = parser.previous;
            params[count].hasDefault = false;
            params[count].isVariadic = isVariadic;

            if (isVariadic) {
                seenVariadic = true;
                if (match(TOKEN_EQUAL)) {
                    errorAt(&parser.previous, "Variadic parameter cannot have a default value.");
                    return count;
                }
            } else if (match(TOKEN_EQUAL)) {
                seenDefault = true;
                if (!parseConstantDefault(&params[count].defaultVal)) {
                    return count;
                }
                params[count].hasDefault = true;
            } else if (seenDefault) {
                errorAt(&parser.previous,
                    "Required parameters must come before parameters with defaults.");
                return count;
            }
            count++;
        } while (match(TOKEN_COMMA));
    }
    return count;
}

// After endCompiler() returns fn, wire up the default values.
// requiredArity is the number of params WITHOUT a default.
void attachDefaults(ObjFunction* fn, ParamInfo* params, int paramCount, int requiredArity) {
    int defaultCount = paramCount - requiredArity;
    if (defaultCount == 0) return;

    fn->requiredArity = requiredArity;
    fn->defaultCount  = defaultCount;
    fn->defaults = (Value*)reallocate(NULL, 0, sizeof(Value) * (size_t)defaultCount);
    for (int i = 0; i < defaultCount; i++) {
        fn->defaults[i] = params[requiredArity + i].defaultVal;
    }
}

// ── Lambda / anonymous function expression ────────────────────────────────
// Two supported forms:
//
//   Arrow body (expression result):
//     fn(x, y) => x + y
//
//   Block body (same indented block as a named fn, returns nil unless
//   an explicit `return` is hit):
//     fn(x, y)
//         let z = x + y
//         return z
//
// The TOKEN_FN prefix rule lands here whenever `fn` appears in expression
// position — i.e. wherever a value is expected (right-hand side of `let`,
// inside a call's argument list, as the target of `spawn`, etc.).
void lambdaExpr(void) {
    // TOKEN_FN was already consumed by parsePrecedence().
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'fn' in lambda expression.");

    // ── Parse parameter list ──────────────────────────────────────────────
    ParamInfo params[256];
    int arity = parseParamList(params);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after lambda parameters.");

    // ── Create a sub-compiler for the lambda's body chunk ─────────────────
    // Anonymous lambdas get the synthetic name "<lambda>" so stack traces
    // and future debuggers can identify them without a real identifier.
    ObjString* lambdaName = copyString("<lambda>", 8);

    Compiler compiler;
    initCompiler(&compiler, lambdaName, arity);

    beginScope();

    // Dummy slot so argument indices align with slotsIndex + 1
    // (same convention as fnDeclaration and method).
    Token dummyToken = syntheticToken("", 0, TOKEN_IDENTIFIER);
    addLocal(dummyToken, false);

    // Count required (non-defaulted) params
    int requiredArity = 0;
    while (requiredArity < arity && !params[requiredArity].hasDefault) requiredArity++;

    for (int i = 0; i < arity; i++) {
        addLocal(params[i].name, false);
    }

    // ── Body: arrow form  =>  or  block form  NEWLINE INDENT ... DEDENT ──
    // The scanner has no TOKEN_ARROW, so `=>` arrives as TOKEN_EQUAL ('=')
    // followed by TOKEN_GREATER ('>'). We peek at current, then consume both.
    if (parser.current.type == TOKEN_EQUAL) {
        advance(); // consume '='
        consume(TOKEN_GREATER, "Expect '>' after '=' in lambda arrow '=>'.");
        // Arrow body: compile the expression, then emit an explicit return.
        expression();
        emitByte(OP_RETURN_VAL);
        lastExprEndedInBlock = false;
    } else if (parser.current.type == TOKEN_NEWLINE) {
        // Block body — same as a named function.
        skipNewlines();
        block();
        // block() already consumed the closing DEDENT for us; tell the
        // enclosing statement() not to also demand a NEWLINE/DEDENT.
        lastExprEndedInBlock = true;
    } else {
        errorAt(&parser.current,
                "Expect '=>' or newline+indented block after lambda parameters.");
    }

    ObjFunction* fn = endCompiler();
    bool hasVariadic = arity > 0 && params[arity - 1].isVariadic;
    fn->isVariadic = hasVariadic;
    if (!hasVariadic) {
        attachDefaults(fn, params, arity, requiredArity);
    }

    // Emit the closure + upvalue capture pairs.
    emitByte(OP_CLOSURE);
    emitShort(makeConstant((Value){VAL_OBJ, {.obj = (Obj*)fn}}));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }
    // The closure value is now on top of the outer stack — the caller
    // (parsePrecedence / expression) will do whatever it wants with it
    // (assign to a variable, pass as an argument, call immediately, etc.).
}

void fnDeclaration(void) {
    if (parser.current.type != TOKEN_IDENTIFIER) {
        errorAt(&parser.current, "Expect function name.");
        return;
    }
    advance();
    Token fnName = parser.previous;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    ParamInfo params[256];
    int arity = parseParamList(params);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_NEWLINE, "Expect newline after function signature.");

    int requiredArity = 0;
    while (requiredArity < arity && !params[requiredArity].hasDefault) requiredArity++;

    ObjString* nameStr = copyString(fnName.start, fnName.length);

    bool isLocalFn = current->scopeDepth > 0;
    if (isLocalFn) {
        addLocal(fnName, false);
    }

    Compiler compiler;
    initCompiler(&compiler, nameStr, arity);

    beginScope();

    // Add dummy local so that arguments align with slotsIndex + 1
    Token dummyToken = syntheticToken("", 0, TOKEN_IDENTIFIER);
    addLocal(dummyToken, false);

    for (int i = 0; i < arity; i++) {
        addLocal(params[i].name, false);
    }

    skipNewlines();
    block();

    ObjFunction* fn = endCompiler();
    bool hasVariadic = arity > 0 && params[arity - 1].isVariadic;
    fn->isVariadic = hasVariadic;
    if (!hasVariadic) {
        attachDefaults(fn, params, arity, requiredArity);
    }

    emitByte(OP_CLOSURE);
    emitShort(makeConstant((Value){VAL_OBJ, {.obj = (Obj*)fn}}));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }

    if (!isLocalFn) {
        uint16_t nameIdx = identifierConstant(&fnName);
        emitByte(OP_DEFINE_LET);
        emitShort(nameIdx);
    }
}

static void method(void) {
    consume(TOKEN_FN, "Expect 'fn' before method name.");
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    Token methodNameTok = parser.previous;
    uint16_t nameConstant = identifierConstant(&methodNameTok);

    consume(TOKEN_LEFT_PAREN, "Expect '(' after method name.");

    ParamInfo params[256];
    int arity = parseParamList(params);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_NEWLINE, "Expect newline after method signature.");

    int requiredArity = 0;
    while (requiredArity < arity && !params[requiredArity].hasDefault) requiredArity++;

    ObjString* nameStr = copyString(methodNameTok.start, methodNameTok.length);

    Compiler compiler;
    initCompiler(&compiler, nameStr, arity);
    compiler.isInitializer = identifierEqual(&methodNameTok, "init");

    beginScope();

    // Implicit 'self' variable for methods. Type is TOKEN_SELF (not
    // TOKEN_IDENTIFIER) since that's how the scanner tags a real `self`
    // occurrence; resolveLocalIn() only compares start/length via memcmp,
    // so the type field doesn't affect whether uses of `self` resolve to it.
    Token selfToken = syntheticToken("self", 4, TOKEN_SELF);
    addLocal(selfToken, false);

    for (int i = 0; i < arity; i++) {
        addLocal(params[i].name, false);
    }

    skipNewlines();
    block();

    ObjFunction* fn = endCompiler();
    bool hasVariadic = arity > 0 && params[arity - 1].isVariadic;
    fn->isVariadic = hasVariadic;
    if (!hasVariadic) {
        attachDefaults(fn, params, arity, requiredArity);
    }

    emitByte(OP_CLOSURE);
    emitShort(makeConstant((Value){VAL_OBJ, {.obj = (Obj*)fn}}));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
        emitByte(compiler.upvalues[i].index);
    }

    emitByte(OP_METHOD);
    emitShort(nameConstant);
}

void classDeclaration(void) {
    consume(TOKEN_IDENTIFIER, "Expect class name.");
    Token className = parser.previous;
    uint16_t nameConstant = identifierConstant(&className);

    bool isLocalClass = current->scopeDepth > 0;
    if (isLocalClass) {
        addLocal(className, false);
    }

    emitByte(OP_CLASS);
    emitShort(nameConstant);

    if (!isLocalClass) {
        emitByte(OP_DEFINE_LET);
        emitShort(nameConstant);
    }

    if (isLocalClass) {
        int slot = resolveLocal(&className);
        emitByte(OP_GET_LOCAL);
        emitByte((uint8_t)slot);
    } else {
        emitByte(OP_GET_GLOBAL);
        emitShort(nameConstant);
    }

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect newline after class name.");
        return;
    }
    skipNewlines();
    if (!match(TOKEN_INDENT)) {
        errorAt(&parser.current, "Expect indented block after class declaration.");
        return;
    }

    while (parser.current.type != TOKEN_DEDENT && parser.current.type != TOKEN_EOF) {
        method();
        skipNewlines();
    }
    consume(TOKEN_DEDENT, "Expect dedent to close class block.");

    emitByte(OP_POP);
}
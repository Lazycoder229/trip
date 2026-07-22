// compiler_concurrency.c — spawn/async/await: the fiber-scheduler-facing
// surface of the language. Compiles to plain closures + a couple of
// builtin calls; the scheduler does the real work at runtime.

#include <string.h>
#include "compiler_internal.h"

// ── spawn statement ───────────────────────────────────────────────────────
// Syntax:  spawn expr               — expr must be a 0-arg fn
//          spawn expr(arg1, arg2)   — call with arguments
//
// We parse the function name as a PRIMARY expression only (no call),
// then separately consume the optional argument list. This prevents
// `spawn fn(a)` from being compiled as "call fn(a), then spawn the result".
void spawnStatement(void) {
    // Parse ONLY the function reference — stop before '(' so we don't
    // accidentally compile it as a call. We use unary() which covers
    // identifiers, dot-chains, index expressions, but NOT call expressions.
    // Actually we need just the variable/primary — use a flag approach:
    // parse a primary expression (name lookup only).

    // Emit a plain variable load — handles identifiers, self.method, etc.
    // For simplicity: if next token is an identifier, load it as a variable.
    // More complex expressions (closures, index) still work via expression().
    // The key is we stop BEFORE consuming '(' as a call.
    if (parser.current.type == TOKEN_IDENTIFIER) {
        // Load the variable (closure) onto the stack without calling it.
        Token name = parser.current;
        advance();

        // Check if it's a local first, then global.
        int localIdx = -1;
        for (int i = current->localCount - 1; i >= 0; i--) {
            Local* loc = &current->locals[i];
            if (loc->depth != -1 &&
                loc->name.length == name.length &&
                memcmp(loc->name.start, name.start, name.length) == 0) {
                localIdx = i;
                break;
            }
        }
        if (localIdx != -1) {
            emitByte(OP_GET_LOCAL);
            emitByte((uint8_t)localIdx);
        } else {
            // Global — use identifierConstant + emitShort (2-byte index).
            uint16_t nameConst = identifierConstant(&name);
            emitByte(OP_GET_GLOBAL);
            emitShort(nameConst);
        }
    } else {
        // Fallback: arbitrary expression (lambda, etc.) — caller must not
        // write `spawn (fn(a))(b)` style; this path assumes no trailing call.
        expression();
    }

    // Now consume the optional argument list.
    int argCount = 0;
    if (match(TOKEN_LEFT_PAREN)) {
        if (parser.current.type != TOKEN_RIGHT_PAREN) {
            do {
                skipNewlines();
                expression();
                argCount++;
                if (argCount > 255)
                    errorAt(&parser.current, "spawn() cannot pass more than 255 arguments.");
                skipNewlines();
            } while (match(TOKEN_COMMA));
        }
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after spawn arguments.");
    }

    // Emit: OP_CALL_BUILTIN BUILTIN_SPAWN (1 + argCount)
    emitByte(OP_CALL_BUILTIN);
    emitByte(BUILTIN_SPAWN);
    emitByte((uint8_t)(1 + argCount));
    emitByte(OP_POP); // spawn() pushes nil; discard it
}

// ── async lambda expression ───────────────────────────────────────────────
// Syntax (expression position):
//   async fn(params) => expr
//   async fn(params)
//       body
//
// Identical to a regular lambda (lambdaExpr) but sets isAsync = true on the
// inner compiler so that `await` is legal in the body.
void asyncLambdaExpr(void) {
    // TOKEN_ASYNC was already consumed.
    if (!match(TOKEN_FN)) {
        errorAt(&parser.current, "Expect 'fn' after 'async' in expression.");
        return;
    }
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'fn' in async lambda.");

    ParamInfo params[256];
    int arity = parseParamList(params);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after async lambda parameters.");

    ObjString* lambdaName = copyString("<async lambda>", 14);

    Compiler compiler;
    initCompiler(&compiler, lambdaName, arity);
    compiler.isAsync = true;

    beginScope();

    Token dummyToken = syntheticToken("", 0, TOKEN_IDENTIFIER);
    addLocal(dummyToken, false);

    int requiredArity = 0;
    while (requiredArity < arity && !params[requiredArity].hasDefault) requiredArity++;

    for (int i = 0; i < arity; i++) {
        addLocal(params[i].name, false);
    }

    if (parser.current.type == TOKEN_EQUAL) {
        advance();
        consume(TOKEN_GREATER, "Expect '>' after '=' in async lambda arrow '=>'.");
        expression();
        emitByte(OP_RETURN_VAL);
    } else if (parser.current.type == TOKEN_NEWLINE) {
        skipNewlines();
        block();
    } else {
        errorAt(&parser.current,
                "Expect '=>' or newline+indented block after async lambda parameters.");
    }

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
}

// ── await expression ──────────────────────────────────────────────────────
// Syntax:  await expr
//
// `await` is only valid inside an `async fn`.  It evaluates `expr` (which
// must be a call that returns a value) and then cooperatively yields the
// current fiber, allowing other fibers to run while I/O or other work
// completes.  Because Trip's I/O builtins (httpGet, tcpRead, etc.) already
// integrate with the fiber scheduler internally, `await` compiles to:
//
//   1. Evaluate the awaited expression — leaves its result on the stack.
//   2. Emit yield() — hands control back to the scheduler so other fibers
//      can run; the scheduler immediately re-queues the current fiber since
//      there is no actual blocking condition (the builtin already handled it).
//
// This makes `await` a clear cooperative checkpoint and a syntactic signal
// that the call may do I/O, without requiring a separate promise/future type.
void awaitExpr(void) {
    // TOKEN_AWAIT was already consumed by parsePrecedence().
    // Walk up the compiler chain to confirm we're inside an async fn.
    bool insideAsync = false;
    for (Compiler* c = current; c != NULL; c = c->enclosing) {
        if (c->isAsync) { insideAsync = true; break; }
    }
    if (!insideAsync) {
        errorAt(&parser.previous, "'await' can only be used inside an 'async' function.");
        return;
    }

    // Parse the awaited expression at UNARY precedence so that
    //   await foo(x) + 1
    // is parsed as  (await foo(x)) + 1  rather than  await (foo(x) + 1).
    parsePrecedence(PREC_UNARY);

    // Yield cooperatively so other fibers can run.  yield() in Trip takes
    // no arguments and returns nil; the result we care about is already on
    // the stack from the expression above, so we pop the nil yield() returns.
    emitByte(OP_CALL_BUILTIN);
    emitByte(BUILTIN_YIELD);
    emitByte(0);   // argc = 0
    emitByte(OP_POP);  // discard nil returned by yield()
    // The awaited expression's result remains on the stack as the value of
    // the overall `await` expression.
}

// ── async fn declaration ─────────────────────────────────────────────────
// Syntax:
//   async fn name(params...)
//       body
//
// Compiles identically to a regular `fn`, but sets compiler.isAsync = true
// on the inner compiler so that `await` expressions inside the body are
// permitted.  The function itself is a plain closure at runtime — no wrapper
// object or promise type is needed because Trip's fiber scheduler already
// handles cooperative I/O.
//
// An async function can be called normally:
//   fetchData()           # blocks current fiber until done (fine for scripts)
//   spawn fetchData()     # runs concurrently with other fibers
void asyncFnDeclaration(void) {
    // We enter here right after `async` has been consumed.
    if (!match(TOKEN_FN)) {
        errorAt(&parser.current, "Expect 'fn' after 'async'.");
        return;
    }

    if (parser.current.type != TOKEN_IDENTIFIER) {
        errorAt(&parser.current, "Expect function name after 'async fn'.");
        return;
    }
    advance();
    Token fnName = parser.previous;

    consume(TOKEN_LEFT_PAREN, "Expect '(' after async function name.");

    ParamInfo params[256];
    int arity = parseParamList(params);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
    consume(TOKEN_NEWLINE, "Expect newline after async function signature.");

    int requiredArity = 0;
    while (requiredArity < arity && !params[requiredArity].hasDefault) requiredArity++;

    ObjString* nameStr = copyString(fnName.start, fnName.length);

    bool isLocalFn = current->scopeDepth > 0;
    if (isLocalFn) {
        addLocal(fnName, false);
    }

    Compiler compiler;
    initCompiler(&compiler, nameStr, arity);
    compiler.isAsync = true;   // ← the key difference from a plain fn

    beginScope();

    // Dummy slot so argument indices align with slotsIndex + 1.
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

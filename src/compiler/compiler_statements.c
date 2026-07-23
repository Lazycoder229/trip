// compiler_statements.c — statement()'s dispatch table plus the bodies of
// every statement that doesn't warrant its own file: blocks, if/elif/else,
// while/for loops (and their optional `else` clause), `match`, `break`/
// `continue`, and `let`/`const` declarations.

#include "compiler_internal.h"

// ── Loop bookkeeping ───────────────────────────────────────────────────────
// Only while/for loops (and break/continue/else-clause handling) need this,
// so it stays private to this file rather than living in the shared header.
typedef struct {
    int loopStart;
    int breakCount;
    int breakJumps[256];
    // for/while...else support:
    // elseFlagSlot >= 0 means this loop has an else clause.
    // The slot holds a boolean local (__broke) that starts false and is
    // set to true whenever break fires.  After the loop body, if __broke
    // is still false the else block is executed.
    int elseFlagSlot;
} LoopContext;

static LoopContext* currentLoop = NULL;

static void emitLoopElse(LoopContext* loop);
static void forInCollection(Token varName);

static void whileStatement(void) {
    LoopContext loop;
    loop.breakCount = 0;

    // Allocate __broke local before loop starts so break can set it.
    // It lives at the current (pre-loop) scope level so it survives
    // the body's beginScope/endScope and is still alive for the else check.
    //
    // emitConstant(false) already leaves the value sitting in exactly the
    // stack slot addLocal() is about to claim for it — no SET_LOCAL+POP
    // needed. (That dance used to pop the value right back off, leaving
    // every local declared inside the loop body one stack slot short of
    // where the compiler thought it was — see the fix applied to the
    // for-in/range-for loops' __broke setup for the same bug.)
    beginScope();
    emitConstant(BOOL_VAL(false));
    static const char brokeStr[] = "__broke";
    Token brokeName = syntheticToken(brokeStr, 7, TOKEN_IDENTIFIER);
    addLocal(brokeName, false);
    loop.elseFlagSlot = current->localCount - 1;

    loop.loopStart = compilingChunk->count;

    LoopContext* enclosing = currentLoop;
    currentLoop = &loop;

    expression();

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect a newline after the 'while' condition.");
        currentLoop = enclosing;
        endScope();
        return;
    }
    skipNewlines();

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    int bodyScopeDepth = current->scopeDepth + 1;

    if (!match(TOKEN_INDENT)) {
        errorAt(&parser.current, "Expect an indented block.");
        currentLoop = enclosing;
        endScope();
        return;
    }
    beginScope();
    while (parser.current.type != TOKEN_DEDENT && parser.current.type != TOKEN_EOF) {
        statement();
        skipNewlines();
    }
    closeLoopUpvalues(bodyScopeDepth);
    endScope();
    if (!match(TOKEN_DEDENT)) {
        errorAt(&parser.current, "Expect a dedent to close the block.");
    }

    emitLoop(loop.loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < loop.breakCount; i++) {
        patchJump(loop.breakJumps[i]);
    }

    currentLoop = enclosing;

    skipNewlines();
    emitLoopElse(&loop);

    endScope(); // closes the __broke local
}

static void breakStatement(void) {
    if (currentLoop == NULL) {
        errorAt(&parser.previous, "'break' used outside of a loop.");
        return;
    }
    if (currentLoop->breakCount >= 256) {
        errorAt(&parser.previous, "Too many 'break' statements in one loop.");
        return;
    }
    // If this loop has an else clause, set __broke = true before jumping out.
    if (currentLoop->elseFlagSlot >= 0) {
        emitConstant(BOOL_VAL(true));
        emitByte(OP_SET_LOCAL);
        emitByte((uint8_t)currentLoop->elseFlagSlot);
        emitByte(OP_POP);
    }
    int jump = emitJump(OP_JUMP);
    currentLoop->breakJumps[currentLoop->breakCount++] = jump;
}

static void continueStatement(void) {
    if (currentLoop == NULL) {
        errorAt(&parser.previous, "'continue' used outside of a loop.");
        return;
    }
    emitLoop(currentLoop->loopStart);
}

// Emit the optional else block for a for/while loop.
// Called after the loop body and break patches are done.
// If there is no 'else' keyword the function is a no-op.
// If there is one, it emits:
//   if (__broke) jump over else body
//   <else body>
//   <patch skip-jump here>
// The __broke local is part of the loop's own scope and will be cleaned
// up by the caller's endScope() / pop sequence.
static void emitLoopElse(LoopContext* loop) {
    if (!match(TOKEN_ELSE)) return;

    // __broke must have been set up by the caller.
    if (loop->elseFlagSlot < 0) return;

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect newline after 'else'.");
        return;
    }
    skipNewlines();

    // Load __broke; if true (break was hit) jump over the else body.
    emitByte(OP_GET_LOCAL);
    emitByte((uint8_t)loop->elseFlagSlot);
    int skipElse = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // pop the true value — we are skipping else
    int overElse = emitJump(OP_JUMP);
    patchJump(skipElse);
    emitByte(OP_POP); // pop the false value — we run else

    block();

    patchJump(overElse);
}

#define MAX_MATCH_CASES 256

static void matchStatement(void) {
    expression();

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect a newline after 'match' subject.");
        return;
    }
    skipNewlines();

    if (!match(TOKEN_INDENT)) {
        errorAt(&parser.current, "Expect an indented block after 'match'.");
        return;
    }

    int endJumps[MAX_MATCH_CASES];
    int endJumpCount = 0;

    while (parser.current.type == TOKEN_CASE) {
        advance();

        expression();

        emitByte(OP_MATCH_EQUAL);

        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after 'case' value.");
            break;
        }
        skipNewlines();

        int skipJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);

        block();

        emitByte(OP_POP);

        if (endJumpCount >= MAX_MATCH_CASES) {
            errorAt(&parser.previous, "Too many cases in one match.");
            break;
        }
        endJumps[endJumpCount++] = emitJump(OP_JUMP);

        patchJump(skipJump);
        emitByte(OP_POP);

        skipNewlines();
    }

    if (match(TOKEN_ELSE)) {
        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after 'else'.");
        } else {
            skipNewlines();
            block();
        }
        emitByte(OP_POP);
    } else {
        emitByte(OP_POP);
    }

    if (!match(TOKEN_DEDENT)) {
        errorAt(&parser.current, "Expect a dedent to close 'match' block.");
    }

    for (int i = 0; i < endJumpCount; i++) {
        patchJump(endJumps[i]);
    }
}

// ── Variable declarations ─────────────────────────────────────────────────

static void letDeclaration(void) {
    if (parser.current.type != TOKEN_IDENTIFIER) {
        errorAt(&parser.current, "Expect variable name after 'let'.");
        return;
    }
    advance();
    Token name = parser.previous;

    if (!match(TOKEN_EQUAL)) {
        errorAt(&parser.current, "Expect '=' after variable name.");
        return;
    }
    expression();

    if (current->scopeDepth > 0) {
        addLocal(name, false);
    } else {
        emitByte(OP_DEFINE_LET);
        emitShort(identifierConstant(&name));
    }
}

static void constDeclaration(void) {
    if (parser.current.type != TOKEN_IDENTIFIER) {
        errorAt(&parser.current, "Expect variable name after 'const'.");
        return;
    }
    advance();
    Token name = parser.previous;

    if (!match(TOKEN_EQUAL)) {
        errorAt(&parser.current, "Expect '=' after variable name.");
        return;
    }
    expression();

    if (current->scopeDepth > 0) {
        addLocal(name, true);
    } else {
        emitByte(OP_DEFINE_CONST);
        emitShort(identifierConstant(&name));
    }
}

static void returnStatement(void) {
    if (parser.current.type == TOKEN_NEWLINE || parser.current.type == TOKEN_EOF) {
        if (current->isInitializer) {
            emitByte(OP_GET_LOCAL);
            emitByte(0);
            emitByte(OP_RETURN_VAL);
        } else {
            emitByte(OP_RETURN_NIL);
        }
    } else {
        expression();
        emitByte(OP_RETURN_VAL);
    }
}

void block(void) {
    if (!match(TOKEN_INDENT)) {
        errorAt(&parser.current, "Expect an indented block.");
        return;
    }
    beginScope();
    while (parser.current.type != TOKEN_DEDENT && parser.current.type != TOKEN_EOF) {
        statement();
        skipNewlines();
    }
    endScope();
    if (!match(TOKEN_DEDENT)) {
        errorAt(&parser.current, "Expect a dedent to close the block.");
    }
}

static void ifStatement(void) {
    #define MAX_ELIF 64
    int endJumps[MAX_ELIF];
    int endJumpCount = 0;

    expression();

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect a newline after 'if' condition.");
        return;
    }
    skipNewlines();

    int skipJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    block();

    endJumps[endJumpCount++] = emitJump(OP_JUMP);
    patchJump(skipJump);
    emitByte(OP_POP);

    skipNewlines();

    while (parser.current.type == TOKEN_ELIF) {
        advance();
        expression();

        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after 'elif' condition.");
            break;
        }
        skipNewlines();

        int elifSkip = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
        block();

        if (endJumpCount < MAX_ELIF) {
            endJumps[endJumpCount++] = emitJump(OP_JUMP);
        }
        patchJump(elifSkip);
        emitByte(OP_POP);

        skipNewlines();
    }

    if (match(TOKEN_ELSE)) {
        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after 'else'.");
        } else {
            skipNewlines();
            block();
        }
    }

    for (int i = 0; i < endJumpCount; i++) {
        patchJump(endJumps[i]);
    }
    #undef MAX_ELIF
}

static void forInCollection(Token varName) {
    // Own scope for __broke/__col/__i/varName — mirrors the range-for
    // variant above. Without this, these locals share the enclosing
    // block's scope depth, and the endScope() below pops back through
    // whatever was declared earlier in that block (e.g. a variable set
    // up before the loop), corrupting the stack.
    beginScope();

    // The collection value is already on the stack (pushed by the
    // expression() call in forStatement before we got here). Register it
    // as __col FIRST, directly — it's already sitting in exactly the slot
    // addLocal will assign it, so no push/SET_LOCAL is needed. This MUST
    // happen before __broke is declared: __broke's compiler-assigned slot
    // index is derived purely from localCount, so if it were declared
    // first (while this collection value sits on the stack unregistered),
    // its index would collide with the collection's actual position, and
    // the SET_LOCAL+POP dance previously used to "insert" it there would
    // instead overwrite/destroy the collection value.
    static const char colStr[] = "__col";
    Token colName = syntheticToken(colStr, 5, TOKEN_IDENTIFIER);
    addLocal(colName, false);
    int colSlot = current->localCount - 1;

    // __broke flag for else clause — pushed fresh now that __col is
    // properly accounted for, so its slot naturally matches where it
    // lands on the stack.
    emitConstant(BOOL_VAL(false));
    static const char brokeStr[] = "__broke";
    Token brokeName = syntheticToken(brokeStr, 7, TOKEN_IDENTIFIER);
    addLocal(brokeName, false);
    int brokeSlot = current->localCount - 1;

    emitConstant(NUMBER_VAL(0));
    static const char idxStr[] = "__i";
    Token idxName = syntheticToken(idxStr, 3, TOKEN_IDENTIFIER);
    addLocal(idxName, false);
    int idxSlot = current->localCount - 1;

    emitByte(OP_NIL);
    addLocal(varName, false);
    int varSlot = current->localCount - 1;

    LoopContext loop;
    loop.breakCount = 0;
    loop.elseFlagSlot = brokeSlot;
    LoopContext* enclosing = currentLoop;
    currentLoop = &loop;

    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect newline after collection.");
        currentLoop = enclosing;
        return;
    }
    skipNewlines();

    loop.loopStart = compilingChunk->count;

    emitByte(OP_GET_LOCAL); emitByte((uint8_t)idxSlot);
    emitByte(OP_GET_LOCAL); emitByte((uint8_t)colSlot);
    emitByte(OP_CALL_METHOD);
    emitByte(METHOD_LIST_LEN);
    emitByte(0);
    emitByte(OP_LESS);

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    emitByte(OP_GET_LOCAL); emitByte((uint8_t)colSlot);
    emitByte(OP_GET_LOCAL); emitByte((uint8_t)idxSlot);
    emitByte(OP_INDEX_GET);
    emitByte(OP_SET_LOCAL);
    emitByte((uint8_t)varSlot);
    emitByte(OP_POP);

    block();

    emitByte(OP_GET_LOCAL); emitByte((uint8_t)idxSlot);
    emitConstant(NUMBER_VAL(1));
    emitByte(OP_ADD);
    emitByte(OP_SET_LOCAL); emitByte((uint8_t)idxSlot);
    emitByte(OP_POP);

    emitLoop(loop.loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < loop.breakCount; i++) {
        patchJump(loop.breakJumps[i]);
    }

    currentLoop = enclosing;

    skipNewlines();
    emitLoopElse(&loop);

    endScope();
}

static void forStatement(void) {
    if (parser.current.type != TOKEN_IDENTIFIER) {
        errorAt(&parser.current, "Expect loop variable name after 'for'.");
        return;
    }
    advance();
    Token varName = parser.previous;

    if (!match(TOKEN_IN)) {
        errorAt(&parser.current, "Expect 'in' after loop variable.");
        return;
    }

    expression();

    if (match(TOKEN_DOTDOT)) {
        // ── Range for: for x in start..end ───────────────────────────────
        // Own scope: holds varName, __broke, __end.
        beginScope();

        // start value is already on the stack — bind it to varName FIRST.
        // It must be registered before __broke: __broke's compiler slot
        // index comes purely from localCount, so declaring it first (while
        // this start value sits on the stack unregistered) would make its
        // index collide with the start value's actual position — the old
        // SET_LOCAL+POP dance used to "insert" it there ended up
        // overwriting/destroying the start value instead.
        addLocal(varName, false);
        int varSlot = current->localCount - 1;

        // __broke flag for else clause — pushed fresh now that varName is
        // accounted for, so its slot naturally matches where it lands.
        emitConstant(BOOL_VAL(false));
        static const char brokeStr[] = "__broke";
        Token brokeName = syntheticToken(brokeStr, 7, TOKEN_IDENTIFIER);
        addLocal(brokeName, false);

        LoopContext loop;
        loop.breakCount   = 0;
        loop.elseFlagSlot = current->localCount - 1;

        expression();
        static const char endStr[] = "__end";
        Token endName = syntheticToken(endStr, 5, TOKEN_IDENTIFIER);
        addLocal(endName, false);
        int endSlot = current->localCount - 1;

        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect newline after range.");
            endScope();
            return;
        }
        skipNewlines();

        LoopContext* enclosing = currentLoop;
        currentLoop = &loop;

        loop.loopStart = compilingChunk->count;
        emitByte(OP_GET_LOCAL); emitByte((uint8_t)varSlot);
        emitByte(OP_GET_LOCAL); emitByte((uint8_t)endSlot);
        emitByte(OP_LESS);

        int exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);

        block();

        emitByte(OP_GET_LOCAL); emitByte((uint8_t)varSlot);
        emitConstant(NUMBER_VAL(1));
        emitByte(OP_ADD);
        emitByte(OP_SET_LOCAL); emitByte((uint8_t)varSlot);
        emitByte(OP_POP);

        emitLoop(loop.loopStart);

        patchJump(exitJump);
        emitByte(OP_POP);

        for (int i = 0; i < loop.breakCount; i++) {
            patchJump(loop.breakJumps[i]);
        }

        currentLoop = enclosing;

        skipNewlines();
        emitLoopElse(&loop);

        endScope();
    } else {
        // ── Collection for: for x in collection ──────────────────────────
        // forInCollection is entirely self-contained: it opens its own scope,
        // allocates __broke + iteration locals, calls emitLoopElse, endScope.
        // The collection value is already on the stack (from expression() above).
        LoopContext* enclosing = currentLoop;
        forInCollection(varName);
        currentLoop = enclosing;
    }
}

void statement(void) {
    if (match(TOKEN_LET)) {
        letDeclaration();
    } else if (match(TOKEN_CONST)) {
        constDeclaration();
    } else if (match(TOKEN_IF)) {
        ifStatement();
        return;
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
        return;
    } else if (match(TOKEN_CLASS)) {
        classDeclaration();
        return;
    } else if (match(TOKEN_FOR)) {
        forStatement();
        return;
    } else if (match(TOKEN_FN)) {
        fnDeclaration();
        return;
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_MATCH)) {
        matchStatement();
        return;
    } else if (match(TOKEN_BREAK)) {
        breakStatement();
    } else if (match(TOKEN_CONTINUE)) {
        continueStatement();
    } else if (match(TOKEN_TRY)) {
        tryStatement();
        return;
    } else if (match(TOKEN_THROW)) {
        throwStatement();
    } else if (match(TOKEN_IMPORT)) {
        importStatement();
    } else if (match(TOKEN_SPAWN)) {
        spawnStatement();
    } else if (match(TOKEN_ASYNC)) {
        asyncFnDeclaration();
        return;
    } else {
        lastExprWasVoid = false;
        lastExprEndedInBlock = false;
        expression();
        if (!lastExprWasVoid) {
            emitByte(OP_POP);
        }
    }
    // Consume trailing newline. DEDENT is left for the caller to handle
    // (block() / whileStatement() detect it as the end of their body).
    // If the expression was a lambda with a block body, block() already
    // consumed the closing DEDENT itself — nothing left to check here.
    if (lastExprEndedInBlock) {
        // already terminated
    } else if (parser.current.type == TOKEN_NEWLINE) {
        advance();
    } else if (parser.current.type != TOKEN_EOF &&
               parser.current.type != TOKEN_DEDENT) {
        errorAt(&parser.current, "Expect newline after statement.");
    }
}
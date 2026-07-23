// compiler_exceptions.c — try/catch/finally and throw.

#include "compiler_internal.h"

// ── tryStatement ──────────────────────────────────────────────────────────
//
// Grammar:  try NL block (catch ID NL block)? (finally NL block)?
// At least one of catch / finally must be present.
//
// ── Layout A: try + catch  (no finally) ──────────────────────────────────
//
//   OP_TRY_BEGIN  <catch-off> <0xFFFF>
//   [try body]
//   OP_TRY_END
//   OP_JUMP → after
//                                   ← catchIp
//   [catch body]                    ; error value is the local variable
//                                   ← after:
//
// ── Layout B: try + catch + finally ──────────────────────────────────────
//
// Two handlers nested:
//   outer: catches ANYTHING from (try body + catch body), routes to finally
//   inner: catches only errors from try body, routes to catch block
//
//   OP_TRY_BEGIN  <outer-catch-off> <0xFFFF>     ; [OUTER handler]
//   OP_TRY_BEGIN  <inner-catch-off> <0xFFFF>     ; [INNER handler]
//   [try body]
//   OP_TRY_END                                   ; pop INNER on success
//   OP_NIL                                       ; sentinel: no pending error
//   OP_JUMP → finally_entry                      ; skip catch on success
//                                   ← inner catchIp (error from try body)
//   [catch body]                    ; error value on stack as catch local
//   OP_NIL                          ; sentinel: no pending error after catch
//   OP_JUMP → finally_entry         ; fall through to finally
//                                   ← outer catchIp (error from try or catch)
//   [falls into finally_entry]      ; outer handler already popped by unwind
//                                   ← finally_entry:
//   OP_FINALLY_BEGIN                ; pending error is on stack
//   [finally body]
//   OP_FINALLY_END                  ; re-raises if pending != nil
//   OP_TRY_END                      ; pop OUTER on non-error paths
//                                   ← after:
//
// Note: on the error path the outer handler is popped by unwindToHandler,
// so the OP_TRY_END at the end only fires on the normal/caught paths.
//
// ── Layout C: try + finally  (no catch) ──────────────────────────────────
//
//   OP_TRY_BEGIN  <finally-off> <0xFFFF>
//   [try body]
//   OP_TRY_END
//   OP_NIL                          ; sentinel: no pending error
//   OP_JUMP → finally_entry         ; (lands right at finally_entry anyway)
//                                   ← catchIp = finally_entry:
//   OP_FINALLY_BEGIN
//   [finally body]
//   OP_FINALLY_END
//   OP_TRY_END                      ; pop handler on non-error path
//                                   ← after:
//
// OP_TRY_BEGIN always carries 4 operand bytes: 2 for catch offset +
// 2 for finally offset (always 0xFFFF here — we don't use the finally
// field in TryHandler, we route everything through catchIp).
void tryStatement(void) {
    if (!match(TOKEN_NEWLINE)) {
        errorAt(&parser.current, "Expect a newline after 'try'.");
        return;
    }
    skipNewlines();

    // ── Peek ahead: is there a finally block? ────────────────────────────
    // We save the full parser + scanner state, scan past the try body and
    // optional catch block to see if TOKEN_FINALLY follows, then restore.
    // This lets us choose the right layout (1 handler vs 2) before emitting.
    Scanner savedScanner = scanner;
    Parser  savedParser  = parser;

    bool hasFinally = false;
    {
        // Skip to end of try block (past INDENT...DEDENT at depth 1).
        // We consume an INDENT then scan until the matching DEDENT.
        // Simple approach: scan until we see TOKEN_CATCH or TOKEN_FINALLY or
        // TOKEN_EOF at the top level (bracketDepth==0, no extra indent).
        // Since the scanner already emits INDENT/DEDENT we just count them.
        // Fast path: scan tokens looking for FINALLY/CATCH at indent depth 0.
        int depth = 0;
        Token t;
        // consume the INDENT of the try body first
        do { t = scanToken(); } while (t.type == TOKEN_NEWLINE);
        if (t.type == TOKEN_INDENT) depth = 1;
        while (t.type != TOKEN_EOF) {
            t = scanToken();
            if (t.type == TOKEN_INDENT)  depth++;
            if (t.type == TOKEN_DEDENT)  { depth--; if (depth == 0) break; }
        }
        // Now skip optional catch block the same way
        do { t = scanToken(); } while (t.type == TOKEN_NEWLINE);
        if (t.type == TOKEN_CATCH) {
            // skip catch variable + its block
            do { t = scanToken(); } while (t.type != TOKEN_INDENT && t.type != TOKEN_EOF);
            depth = 1;
            while (t.type != TOKEN_EOF) {
                t = scanToken();
                if (t.type == TOKEN_INDENT)  depth++;
                if (t.type == TOKEN_DEDENT)  { depth--; if (depth == 0) break; }
            }
            do { t = scanToken(); } while (t.type == TOKEN_NEWLINE);
        }
        if (t.type == TOKEN_FINALLY) hasFinally = true;
    }
    // Restore scanner + parser to where they were.
    scanner = savedScanner;
    parser  = savedParser;

    // ── Emit handlers ─────────────────────────────────────────────────────
    int outerCatchPh = -1;

    if (hasFinally) {
        // Outer handler: catches anything from try+catch, routes to finally.
        emitByte(OP_TRY_BEGIN);
        outerCatchPh = compilingChunk->count;
        emitByte(0xff); emitByte(0xff);
        emitByte(0xff); emitByte(0xff);
    }

    // Inner/only handler: catches errors from try body, routes to catch.
    emitByte(OP_TRY_BEGIN);
    int innerCatchPh = compilingChunk->count;
    emitByte(0xff); emitByte(0xff);
    emitByte(0xff); emitByte(0xff);

    block();  // ── try body ─────────────────────────────────────────────

    emitByte(OP_TRY_END);   // pop inner handler — try body succeeded

    if (hasFinally) {
        emitByte(OP_NIL);   // sentinel: no pending error
    }

    int skipCatchJump = emitJump(OP_JUMP);  // skip catch block on success

    // ── Patch inner handler's catch offset ───────────────────────────────
    int innerCatchStart  = compilingChunk->count;
    int innerCatchOffset = innerCatchStart - innerCatchPh - 2;
    if (innerCatchOffset > UINT16_MAX) {
        errorAt(&parser.previous, "Too much code in try block."); return;
    }
    compilingChunk->code[innerCatchPh]     = (innerCatchOffset >> 8) & 0xff;
    compilingChunk->code[innerCatchPh + 1] = innerCatchOffset & 0xff;

    skipNewlines();

    bool hasCatch = false;
    int  skipFromCatchJump = -1;

    if (match(TOKEN_CATCH)) {
        hasCatch = true;
        consume(TOKEN_IDENTIFIER, "Expect an error variable name after 'catch'.");
        Token errName = parser.previous;

        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after the 'catch' variable.");
            return;
        }
        skipNewlines();

        beginScope();
        if (!match(TOKEN_INDENT)) {
            errorAt(&parser.current, "Expect an indented block after 'catch'.");
            return;
        }
        addLocal(errName, false);
        while (parser.current.type != TOKEN_DEDENT && parser.current.type != TOKEN_EOF) {
            statement();
            skipNewlines();
        }
        endScope();
        if (!match(TOKEN_DEDENT)) {
            errorAt(&parser.current, "Expect a dedent to close the catch block.");
        }

        if (hasFinally) {
            emitByte(OP_NIL);  // sentinel: no pending error after catch
        }
        skipFromCatchJump = emitJump(OP_JUMP);

        skipNewlines();
    }

    // ── Patch outer handler's catch offset (finally entry point) ─────────
    if (hasFinally) {
        int outerCatchStart  = compilingChunk->count;
        int outerCatchOffset = outerCatchStart - outerCatchPh - 2;
        if (outerCatchOffset > UINT16_MAX) {
            errorAt(&parser.previous, "Too much code in try/catch block."); return;
        }
        compilingChunk->code[outerCatchPh]     = (outerCatchOffset >> 8) & 0xff;
        compilingChunk->code[outerCatchPh + 1] = outerCatchOffset & 0xff;
    }

    // ── Patch the success-path and catch-success-path jumps to land here ─
    patchJump(skipCatchJump);
    if (skipFromCatchJump >= 0) patchJump(skipFromCatchJump);

    if (match(TOKEN_FINALLY)) {
        if (!match(TOKEN_NEWLINE)) {
            errorAt(&parser.current, "Expect a newline after 'finally'.");
            return;
        }
        skipNewlines();

        emitByte(OP_FINALLY_BEGIN);

        if (!match(TOKEN_INDENT)) {
            errorAt(&parser.current, "Expect an indented block after 'finally'.");
            return;
        }
        while (parser.current.type != TOKEN_DEDENT && parser.current.type != TOKEN_EOF) {
            statement();
            skipNewlines();
        }
        if (!match(TOKEN_DEDENT)) {
            errorAt(&parser.current, "Expect a dedent to close the finally block.");
        }

        emitByte(OP_FINALLY_END);

        // Pop the OUTER handler on non-error paths — but only when it was
        // actually emitted (i.e. when we have both catch AND finally).
        // For try+finally-only (Layout C), only one handler was pushed and it
        // was already popped by OP_TRY_END in the try body success path.
        if (hasCatch) {
            emitByte(OP_TRY_END);
        }

    } else {
        // try + catch only (no finally): pop the single (inner) handler on
        // all paths — inner was already popped by OP_TRY_END in the try body
        // and by unwindToHandler on the error path. The outer handler was
        // never emitted. Just pop the NIL we never pushed (we skipped it
        // above when hasFinally was false). Nothing to do.
        if (!hasCatch) {
            errorAt(&parser.previous, "Expect 'catch' or 'finally' after try block.");
        }
    }
}

void throwStatement(void) {
    expression();
    emitByte(OP_THROW);
}

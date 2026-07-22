// compiler.c — the compiler's single public entry point. Everything else
// lives in the compiler_*.c modules (see compiler_internal.h for the map):
//
//   compiler_core.c         parser navigation + bytecode emission
//   compiler_scope.c        Compiler struct / locals / upvalues
//   compiler_expressions.c  Pratt parser (literals, operators, calls)
//   compiler_statements.c   statement() dispatch, blocks, loops, match
//   compiler_functions.c    fn/lambda/class/method declarations
//   compiler_exceptions.c   try/catch/finally/throw
//   compiler_modules.c      import
//   compiler_concurrency.c  spawn/async/await
//
// compile() itself just wires up the root Compiler, primes the scanner,
// and drives the statement loop until EOF.

#include "compiler_internal.h"

bool compile(const char* source, Chunk* chunk) {
    initScanner(source);
    topLevelChunk = chunk;
    compilingChunk = chunk;

    const char* scriptPath = vmGetScriptPath();
    modulesBeginScript(scriptPath);

    Compiler rootCompiler;
    rootCompiler.enclosing  = NULL;
    rootCompiler.function   = NULL;
    rootCompiler.chunk      = chunk;
    rootCompiler.localCount = 0;
    rootCompiler.scopeDepth = 0;
    current = &rootCompiler;

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    // ── Main parse loop ───────────────────────────────────────────────────
    // skipNewlinesAndDedents() instead of skipNewlines() because a multiline
    // expression (e.g. a function call whose argument list spans several
    // indented lines) causes the scanner to emit a TOKEN_DEDENT when
    // indentation returns to the base level after the closing ')'. That
    // DEDENT is not the end of any block — it's just whitespace noise at
    // the top level — and must be discarded before we try to parse the
    // next statement, otherwise parsePrecedence() sees TOKEN_DEDENT as a
    // prefix token (which has no rule) and emits "Expect expression".
    skipNewlinesAndDedents();

    while (parser.current.type != TOKEN_EOF) {
        statement();
        skipNewlinesAndDedents();
    }

    emitReturn();
    return !parser.hadError;
}

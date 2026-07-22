#ifndef compiler_internal_h
#define compiler_internal_h

// ── Internal compiler header ──────────────────────────────────────────────
// Shared by every compiler_*.c translation unit. NOT a public header — do
// not #include this from anything outside the compiler itself (main.c,
// vm.c, etc. only ever need compiler.h's single `compile()` entry point).
//
// This is the glue that lets the compiler be split into focused files while
// still sharing one Parser, one "current Compiler*", and one bytecode
// emitter, exactly as it behaved when everything lived in one 2900-line
// compiler.c. Every extern global and cross-file function below used to be
// a file-scope `static` in that single translation unit.

#include <stdbool.h>
#include <stdint.h>
#include "compiler.h"
#include "../scanner.h"
#include "../chunk.h"
#include "../value.h"
#include "../object.h"
#include "../vm.h"
#include "../memory.h"

// ── Parser state ──────────────────────────────────────────────────────────
typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

extern Parser parser;

// Precedence levels from lowest to highest
typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,   // =
    PREC_OR,           // or
    PREC_AND,          // and
    PREC_EQUALITY,     // == !=
    PREC_COMPARISON,   // < > <= >=
    PREC_TERM,         // + -
    PREC_FACTOR,       // * /
    PREC_UNARY,        // - !
    PREC_PRIMARY
} Precedence;

// The chunk currently being written to (top-level chunk, or a function's
// chunk while compiling its body). Swapped by initCompiler()/endCompiler().
extern Chunk* compilingChunk;

// The chunk passed into compile() — restored as compilingChunk once every
// nested function compiler has unwound back to the root.
extern Chunk* topLevelChunk;

// Set true by `out`/`outn` calls so statement() knows not to emit an extra
// OP_POP for a call whose result was never pushed.
extern bool lastExprWasVoid;

// ── Local variable / upvalue tracking ─────────────────────────────────────
#define MAX_LOCALS 256

typedef struct {
    Token name;
    int  depth;
    bool isConst;
    bool isCaptured;
} Local;

typedef struct {
    uint8_t index;
    bool    isLocal;
    bool    isConst;
} UpvalueInfo;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    Chunk* chunk;

    Local locals[MAX_LOCALS];
    int localCount;
    int scopeDepth;

    UpvalueInfo upvalues[MAX_LOCALS];
    bool isInitializer;  // true when compiling a class's init() method
    bool isAsync;        // true when compiling an async fn — enables `await`
} Compiler;

// The compiler for whatever function (or the top level) is currently being
// compiled. Forms a linked list via ->enclosing while nested functions are
// being compiled, letting upvalue resolution walk outward.
extern Compiler* current;

// ── Default-parameter parsing (shared by compiler_functions.c and
// compiler_concurrency.c, since async fn/lambda declarations parse
// parameter lists identically to regular ones) ────────────────────────────
typedef struct {
    Token name;
    bool  hasDefault;
    Value defaultVal;  // valid only when hasDefault == true
    bool  isVariadic;
} ParamInfo;

// ── compiler_core.c ────────────────────────────────────────────────────────
// Parser navigation, error reporting, and raw bytecode emission — the
// lowest-level primitives every other module is built on.
void errorAt(Token* token, const char* message);
void advance(void);
void consume(TokenKind type, const char* message);
bool match(TokenKind type);

void emitByte(uint8_t byte);
void emitShort(uint16_t value);
void emitReturn(void);
int  emitJump(uint8_t instruction);
void emitLoop(int loopStart);
void patchJump(int offset);
uint16_t makeConstant(Value value);
void emitConstant(Value value);
uint16_t identifierConstant(Token* name);

Token syntheticToken(const char* start, int length, TokenKind type);

void skipNewlines(void);
void skipNewlinesAndDedents(void);

// ── compiler_scope.c ───────────────────────────────────────────────────────
// Compiler struct lifecycle, scope depth, and local/upvalue resolution.
void beginScope(void);
void endScope(void);
void closeLoopUpvalues(int depth);
int  resolveLocal(Token* name);
int  resolveUpvalue(Token* name);
void addLocal(Token name, bool isConst);
void initCompiler(Compiler* compiler, ObjString* name, int arity);
ObjFunction* endCompiler(void);

// ── compiler_expressions.c ────────────────────────────────────────────────
// The Pratt parser: literals, operators, calls, and the ParseRule table.
void expression(void);
void parsePrecedence(Precedence precedence);
ObjString* copyStringWithEscapes(const char* raw, int rawLength);
bool identifierEqual(const Token* token, const char* text);

// ── compiler_statements.c ─────────────────────────────────────────────────
// Statement dispatch and block/loop/conditional/match/declaration bodies.
void statement(void);
void block(void);

// ── compiler_functions.c ──────────────────────────────────────────────────
// Named/anonymous function compilation: parameter lists, defaults,
// fn declarations, lambdas, classes and methods.
int  parseParamList(ParamInfo params[256]);
void attachDefaults(ObjFunction* fn, ParamInfo* params, int paramCount, int requiredArity);
void lambdaExpr(void);
void fnDeclaration(void);
void classDeclaration(void);

// ── compiler_exceptions.c ─────────────────────────────────────────────────
void tryStatement(void);
void throwStatement(void);

// ── compiler_modules.c ────────────────────────────────────────────────────
// import statement + the resolver/dedup state it needs across a whole
// compile() call.
void resetImportTracking(void);
// Called once at the start of compile(): resets import tracking for a
// fresh script and seeds it with the running script's own path (so it
// can't accidentally re-import itself), deriving the base directory that
// relative imports resolve against.
void modulesBeginScript(const char* scriptPath);
void importStatement(void);

// ── compiler_concurrency.c ────────────────────────────────────────────────
void spawnStatement(void);
void asyncLambdaExpr(void);
void awaitExpr(void);
void asyncFnDeclaration(void);

#endif

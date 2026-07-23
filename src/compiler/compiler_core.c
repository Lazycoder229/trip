// compiler_core.c — parser navigation, error reporting, and the
// lowest-level bytecode emitters. Every other compiler_*.c file is built
// on top of these primitives.

#include <stdio.h>
#include <string.h>
#include "compiler_internal.h"

Parser parser;
Chunk* compilingChunk;
Chunk* topLevelChunk = NULL;
bool   lastExprWasVoid = false;
bool   lastExprEndedInBlock = false;

// ── Error reporting ────────────────────────────────────────────────────────
void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    printf("[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        printf(" at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing
    } else {
        printf(" at '%.*s'", token->length, token->start);
    }
    printf(": %s\n", message);
    parser.hadError = true;
}

// ── Token stream navigation ────────────────────────────────────────────────
void advance(void) {
    parser.previous = parser.current;
    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;
        errorAt(&parser.current, parser.current.start);
    }
}

void consume(TokenKind type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAt(&parser.current, message);
}

bool match(TokenKind type) {
    if (parser.current.type != type) return false;
    advance();
    return true;
}

// Builds a Token for a compiler-generated local (dummy call-frame slot,
// implicit `self`, hidden loop counters, etc.) that doesn't correspond to
// any real source text. Always sets every field explicitly — hand-building
// these token-by-token at each call site previously left `.line`
// uninitialized, which is harmless only as long as nothing ever reads it
// (e.g. a future "already declared" diagnostic on a Local's name).
Token syntheticToken(const char* start, int length, TokenKind type) {
    Token token;
    token.start  = start;
    token.length = length;
    token.type   = type;
    token.line   = parser.previous.line; // line of the surrounding source construct
    return token;
}

// ── Whitespace helpers ────────────────────────────────────────────────────

// Skip only newlines — used INSIDE blocks where DEDENT is meaningful.
void skipNewlines(void) {
    while (parser.current.type == TOKEN_NEWLINE) {
        advance();
    }
}

// Skip newlines AND stray dedents — used at the TOP LEVEL and inside
// import bodies, where a DEDENT left over from a multiline expression
// (e.g. a function call whose arguments span several indented lines)
// is not the end of any real block and should be discarded.
void skipNewlinesAndDedents(void) {
    while (parser.current.type == TOKEN_NEWLINE ||
           parser.current.type == TOKEN_DEDENT) {
        advance();
    }
}

// --- Bytecode Emission ---

void emitByte(uint8_t byte) {
    writeChunk(compilingChunk, byte, parser.previous.line);
}

void emitShort(uint16_t value) {
    emitByte((value >> 8) & 0xff);
    emitByte(value & 0xff);
}

void emitReturn(void) {
    emitByte(OP_RETURN);
}

int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return compilingChunk->count - 2;
}

void emitLoop(int loopStart) {
    emitByte(OP_LOOP);

    int offset = compilingChunk->count - loopStart + 2;
    if (offset > UINT16_MAX) {
        errorAt(&parser.previous, "Loop body too large.");
    }

    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
}

void patchJump(int offset) {
    int jump = compilingChunk->count - offset - 2;

    if (jump > UINT16_MAX) {
        errorAt(&parser.previous, "Too much code to jump over.");
    }

    compilingChunk->code[offset]     = (jump >> 8) & 0xff;
    compilingChunk->code[offset + 1] = jump & 0xff;
}

uint16_t makeConstant(Value value) {
    int constant = addConstant(compilingChunk, value);
    if (constant > UINT16_MAX) {
        errorAt(&parser.previous, "Too many constants in one chunk.");
        return 0;
    }
    return (uint16_t)constant;
}

void emitConstant(Value value) {
    emitByte(OP_CONSTANT);
    emitShort(makeConstant(value));
}

// Looks up (or interns as a constant) the name of a global variable /
// property / method, returning its index into the current chunk's
// constant pool. Shared by every module that emits OP_*_GLOBAL,
// OP_PROPERTY_*, OP_METHOD, etc.
uint16_t identifierConstant(Token* name) {
    for (int i = 0; i < compilingChunk->constants.count; i++) {
        Value existing = compilingChunk->constants.values[i];
        if (existing.type == VAL_OBJ) {
            ObjString* existingStr = (ObjString*)AS_OBJ(existing);
            if (existingStr->obj.type == OBJ_STRING &&
                existingStr->length == name->length &&
                memcmp(existingStr->chars, name->start, name->length) == 0) {
                return (uint16_t)i;
            }
        }
    }
    ObjString* str = copyString(name->start, name->length);
    Value val = {VAL_OBJ, {.obj = (Obj*)str}};
    return makeConstant(val);
}
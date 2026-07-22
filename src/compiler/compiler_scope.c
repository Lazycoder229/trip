// compiler_scope.c — the Compiler struct itself: scope depth tracking,
// local-variable slot allocation, and upvalue resolution across nested
// function compilers.

#include <string.h>
#include "compiler_internal.h"

Compiler* current = NULL;

void beginScope(void) {
    current->scopeDepth++;
}

void closeLoopUpvalues(int depth) {
    for (int i = current->localCount - 1; i >= 0; i--) {
        if (current->locals[i].depth < depth) break;
        if (current->locals[i].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
            current->locals[i].isCaptured = false;
        } else {
            emitByte(OP_POP);
        }
        current->locals[i].depth = -1;
    }
}

void endScope(void) {
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth == current->scopeDepth) {
        if (current->locals[current->localCount - 1].isCaptured) {
            emitByte(OP_CLOSE_UPVALUE);
        } else {
            emitByte(OP_POP);
        }
        current->localCount--;
    }
    current->scopeDepth--;
}

static int resolveLocalIn(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (local->name.length == name->length &&
            memcmp(local->name.start, name->start, name->length) == 0) {
            return i;
        }
    }
    return -1;
}

int resolveLocal(Token* name) {
    return resolveLocalIn(current, name);
}

static int addUpvalue(Compiler* compiler, uint8_t index, bool isLocal, bool isConst) {
    int upvalueCount = compiler->function->upvalueCount;
    for (int i = 0; i < upvalueCount; i++) {
        UpvalueInfo* uv = &compiler->upvalues[i];
        if (uv->index == index && uv->isLocal == isLocal) return i;
    }
    if (upvalueCount >= MAX_LOCALS) {
        errorAt(&parser.previous, "Too many closure variables in one function.");
        return 0;
    }
    compiler->upvalues[upvalueCount].isLocal = isLocal;
    compiler->upvalues[upvalueCount].index   = index;
    compiler->upvalues[upvalueCount].isConst = isConst;
    return compiler->function->upvalueCount++;
}

static int resolveUpvalueIn(Compiler* compiler, Token* name) {
    if (compiler->enclosing == NULL) return -1;

    int local = resolveLocalIn(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].isCaptured = true;
        return addUpvalue(compiler, (uint8_t)local, true, compiler->enclosing->locals[local].isConst);
    }

    int upvalue = resolveUpvalueIn(compiler->enclosing, name);
    if (upvalue != -1) {
        return addUpvalue(compiler, (uint8_t)upvalue, false, compiler->enclosing->upvalues[upvalue].isConst);
    }

    return -1;
}

int resolveUpvalue(Token* name) {
    return resolveUpvalueIn(current, name);
}

void addLocal(Token name, bool isConst) {
    if (current->localCount >= MAX_LOCALS) {
        errorAt(&parser.previous, "Too many local variables in scope.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name       = name;
    local->depth      = current->scopeDepth;
    local->isConst    = isConst;
    local->isCaptured = false;
}

void initCompiler(Compiler* compiler, ObjString* name, int arity) {
    compiler->enclosing = current;
    compiler->function = newFunction(name, arity);
    compiler->chunk = &compiler->function->chunk;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->isInitializer = false;
    compiler->isAsync = false;
    current = compiler;
    compilingChunk = compiler->chunk;
}

ObjFunction* endCompiler(void) {
    if (current->isInitializer) {
        // init() implicitly returns `self` (local slot 0), not nil —
        // otherwise the instance the caller just built gets clobbered
        // by OP_RETURN_NIL truncating the stack back to this frame's
        // base (which IS the self slot) and pushing nil there instead.
        emitByte(OP_GET_LOCAL);
        emitByte(0);
        emitByte(OP_RETURN_VAL);
    } else {
        emitByte(OP_RETURN_NIL);
    }
    ObjFunction* function = current->function;
    current = current->enclosing;
    if (current != NULL) {
        compilingChunk = current->chunk;
    } else {
        compilingChunk = topLevelChunk;
    }
    return function;
}

#include <stdio.h>
#include <stdlib.h>
#include "chunk.h"
#include "memory.h"

void initChunk(Chunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    initValueArray(&chunk->constants);
}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->code = (uint8_t*)reallocate(chunk->code,
                                           sizeof(uint8_t) * (size_t)oldCapacity,
                                           sizeof(uint8_t) * (size_t)newCapacity);
        chunk->lines = (int*)reallocate(chunk->lines,
                                        sizeof(int) * (size_t)oldCapacity,
                                        sizeof(int) * (size_t)newCapacity);
        chunk->capacity = newCapacity;
    }
    chunk->code[chunk->count]  = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int getLine(const Chunk* chunk, int offset) {
    if (offset < 0 || offset >= chunk->count) return -1;
    return chunk->lines[offset];
}

int addConstant(Chunk* chunk, Value value) {
    writeValueArray(&chunk->constants, value);
    return chunk->constants.count - 1;
}

void freeChunk(Chunk* chunk) {
    reallocate(chunk->code,  sizeof(uint8_t) * (size_t)chunk->capacity, 0);
    reallocate(chunk->lines, sizeof(int)     * (size_t)chunk->capacity, 0);
    freeValueArray(&chunk->constants);
    initChunk(chunk);
}
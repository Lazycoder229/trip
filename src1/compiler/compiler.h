#ifndef compiler_h
#define compiler_h

#include "../chunk.h"
#include <stdbool.h>

// Takes the raw string, compiles it, and writes the bytes into the chunk
bool compile(const char* source, Chunk* chunk);

#endif
#ifndef memory_h
#define memory_h

#include <stddef.h>
#include "object.h"

// Core allocator — used by all heap operations.
// When newSize == 0, frees the pointer.
// When oldSize == 0 and pointer == NULL, allocates fresh memory.
// Also triggers the GC when memory pressure rises.
void* reallocate(void* pointer, size_t oldSize, size_t newSize);

// ── Mark-and-Sweep GC ────────────────────────────────────────────────────

// Push an object onto the gray worklist (mark it gray).
// Called during the mark phase whenever we discover a reachable object
// that we haven't yet traced through.
void markObject(Obj* object);

// Convenience: marks the object inside a Value (if it IS_OBJ).
void markValue(Value value);

// Run a full GC cycle: mark roots → trace → sweep.
void collectGarbage();

#endif
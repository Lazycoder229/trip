#ifndef table_h
#define table_h

#include <stdbool.h>
#include <stdint.h>
#include "value.h"

typedef struct {
    char* key;
    Value value;
} Entry;

typedef struct {
    int    count;
    int    capacity;
    Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(Table* table);
bool tableSet(Table* table, char* key, Value value);
bool tableGet(Table* table, char* key, Value* value);
bool tableDelete(Table* table, char* key);

// Same-length FNV-1a, exposed so ObjString and callers with a
// precomputed hash don't have to duplicate the algorithm.
uint32_t hashStringFNV(const char* key, int length);

// Hash-aware variants — skip the internal hashString() call when the
// caller already has the hash cached (e.g. an interned ObjString).
bool tableSetHashed(Table* table, char* key, uint32_t hash, Value value);
bool tableGetHashed(Table* table, char* key, uint32_t hash, Value* value);
bool tableDeleteHashed(Table* table, char* key, uint32_t hash);

// String-interning support (used by copyString()).
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash);

// GC support: drop dead (unmarked) entries from a *weak* table like the
// intern table, before sweep() frees the underlying objects.
void tableRemoveWhite(Table* table);

#endif
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "table.h"
#include "object.h"     // needed for AS_STRING/IS_OBJ/AS_OBJ in tableFindString/tableRemoveWhite
#include "memory.h"

// ── FNV-1a hash — better distribution than the old DJB2 variant ──────────
uint32_t hashStringFNV(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}

// Old hashString(key) is now just a strlen-bounded call into the shared fn.
static uint32_t hashString(const char* key) {
    return hashStringFNV(key, (int)strlen(key));
}

// ── Sentinel used for deleted (tombstone) slots ───────────────────────────
// We can't use NULL because that marks "empty" (no probe chain here).
// We use a stable static string address that can never be a real key.
static const char TOMBSTONE_KEY[] = "\x00<tombstone>";
#define IS_TOMBSTONE(e) ((e)->key == TOMBSTONE_KEY)

void initTable(Table* table) {
    table->count    = 0;
    table->capacity = 0;
    table->entries  = NULL;
}

void freeTable(Table* table) {
    reallocate(table->entries, sizeof(Entry) * (size_t)table->capacity, 0);
    table->entries = NULL;
    table->count   = 0;
    table->capacity = 0;
}

// Grow or create the entry array when the load factor hits 0.75.
static void tableGrow(Table* table) {
    int newCap = table->capacity < 8 ? 8 : table->capacity * 2;
    Entry* newEntries = (Entry*)reallocate(NULL, 0, sizeof(Entry) * (size_t)newCap);
    // Zero out so all keys start NULL.
    for (int i = 0; i < newCap; i++) { newEntries[i].key = NULL; }

    // Re-insert every live entry into the new array (tombstones are dropped).
    int newCount = 0;
    for (int i = 0; i < table->capacity; i++) {
        Entry* src = &table->entries[i];
        if (src->key == NULL || IS_TOMBSTONE(src)) continue;

        uint32_t idx = hashString(src->key) % (uint32_t)newCap;
        for (;;) {
            Entry* dst = &newEntries[idx];
            if (dst->key == NULL) { *dst = *src; newCount++; break; }
            idx = (idx + 1) % (uint32_t)newCap;
        }
    }
    reallocate(table->entries, sizeof(Entry) * (size_t)table->capacity, 0);
    table->entries  = newEntries;
    table->capacity = newCap;
    table->count    = newCount;
}

bool tableSet(Table* table, char* key, Value value) {
    // Grow before we risk running out of slots (load factor 0.75).
    if (table->count + 1 > table->capacity * 3 / 4) tableGrow(table);

    uint32_t idx = hashString(key) % (uint32_t)table->capacity;
    Entry* tombstone = NULL;

    for (;;) {
        Entry* e = &table->entries[idx];

        if (e->key == NULL) {
            // Truly empty slot — insert here, reclaiming a tombstone if
            // we passed one earlier in the probe chain.
            Entry* target = tombstone ? tombstone : e;
            target->key   = key;
            target->value = value;
            if (!tombstone) table->count++; // tombstone slot was already counted
            return true;
        }
        if (IS_TOMBSTONE(e)) {
            // Remember the first tombstone; keep probing for the key.
            if (!tombstone) tombstone = e;
        } else if (strcmp(e->key, key) == 0) {
            // Update existing entry.
            e->value = value;
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

bool tableGet(Table* table, char* key, Value* value) {
    if (!table->capacity) return false;

    uint32_t idx = hashString(key) % (uint32_t)table->capacity;
    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL)   return false; // end of probe chain
        if (!IS_TOMBSTONE(e) && strcmp(e->key, key) == 0) {
            *value = e->value;
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

bool tableDelete(Table* table, char* key) {
    if (!table->capacity) return false;

    uint32_t idx = hashString(key) % (uint32_t)table->capacity;
    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL) return false;
        if (!IS_TOMBSTONE(e) && strcmp(e->key, key) == 0) {
            e->key = (char*)TOMBSTONE_KEY;  // mark as tombstone, keep chain intact
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

// ── Hash-aware variants ────────────────────────────────────────────────
// Identical probing logic to the functions above, but the caller supplies
// a precomputed hash (e.g. from an interned ObjString) so we skip the
// hashString() pass entirely on every lookup/insert/delete.

bool tableSetHashed(Table* table, char* key, uint32_t hash, Value value) {
    if (table->count + 1 > table->capacity * 3 / 4) tableGrow(table);

    uint32_t idx = hash % (uint32_t)table->capacity;
    Entry* tombstone = NULL;

    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL) {
            Entry* target = tombstone ? tombstone : e;
            target->key   = key;
            target->value = value;
            if (!tombstone) table->count++;
            return true;
        }
        if (IS_TOMBSTONE(e)) {
            if (!tombstone) tombstone = e;
        } else if (strcmp(e->key, key) == 0) {
            e->value = value;
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

bool tableGetHashed(Table* table, char* key, uint32_t hash, Value* value) {
    if (!table->capacity) return false;
    uint32_t idx = hash % (uint32_t)table->capacity;
    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL) return false;
        if (!IS_TOMBSTONE(e) && strcmp(e->key, key) == 0) {
            *value = e->value;
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

bool tableDeleteHashed(Table* table, char* key, uint32_t hash) {
    if (!table->capacity) return false;
    uint32_t idx = hash % (uint32_t)table->capacity;
    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL) return false;
        if (!IS_TOMBSTONE(e) && strcmp(e->key, key) == 0) {
            e->key = (char*)TOMBSTONE_KEY;
            return true;
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

// ── String interning ──────────────────────────────────────────────────
// Looks up an already-interned ObjString by raw content, so copyString()
// can hand back the canonical instance instead of allocating a duplicate.
ObjString* tableFindString(Table* table, const char* chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t idx = hash % (uint32_t)table->capacity;
    for (;;) {
        Entry* e = &table->entries[idx];
        if (e->key == NULL) {
            return NULL;  // truly empty slot — end of probe chain
        } else if (!IS_TOMBSTONE(e) &&
                   (int)strlen(e->key) == length &&
                   memcmp(e->key, chars, length) == 0) {
            return AS_STRING(e->value);
        }
        idx = (idx + 1) % (uint32_t)table->capacity;
    }
}

// ── Weak-table GC support ───────────────────────────────────────────────
// The intern table (vm.strings) holds ObjString* values but must NOT keep
// them alive on its own — otherwise no string would ever be collected.
// Called from collectGarbage() after tracing, before sweep(): any entry
// whose object didn't get marked this cycle is about to be freed, so we
// drop it from the table first (else its char* key would dangle).
void tableRemoveWhite(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* e = &table->entries[i];
        if (e->key != NULL && !IS_TOMBSTONE(e)) {
            if (IS_OBJ(e->value) && !AS_OBJ(e->value)->isMarked) {
                tableDelete(table, e->key);
            }
        }
    }
}
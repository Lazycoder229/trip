// tests/test_table.c
//
// Unit tests for src/table.c (the open-addressing hash table used for
// vm.globals and vm.strings).
//
// table.c itself has no dependency on the VM (it only uses Value/Obj
// macros, never calls into vm.c), so most of these tests exercise it
// completely standalone. The copyString()-based tests additionally link
// object.c + memory.c + tests/test_support.c to get real interning
// behavior (copyString() calls into vm.strings via the table under test).

#include "test.h"
#include "test_support.h"
#include "../src/table.h"
#include "../src/object.h"
#include "../src/value.h"
#include <string.h>

// ── raw Table tests (no VM needed) ────────────────────────────────────────

TEST(set_and_get_roundtrip) {
    Table t;
    initTable(&t);

    tableSet(&t, "answer", NUMBER_VAL(42));

    Value out;
    bool found = tableGet(&t, "answer", &out);
    ASSERT_TRUE(found);
    ASSERT_TRUE(IS_NUMBER(out));
    ASSERT_TRUE(AS_NUMBER(out) == 42);

    freeTable(&t);
}

TEST(get_on_missing_key_returns_false) {
    Table t;
    initTable(&t);

    Value out;
    bool found = tableGet(&t, "nope", &out);
    ASSERT_TRUE(!found);

    freeTable(&t);
}

TEST(set_on_existing_key_updates_value_not_count) {
    Table t;
    initTable(&t);

    tableSet(&t, "x", NUMBER_VAL(1));
    tableSet(&t, "x", NUMBER_VAL(2));
    ASSERT_EQ_INT(1, t.count);

    Value out;
    tableGet(&t, "x", &out);
    ASSERT_TRUE(AS_NUMBER(out) == 2);

    freeTable(&t);
}

TEST(delete_removes_key_but_keeps_probe_chain_intact) {
    // Classic open-addressing regression: deleting a key that's in the
    // middle of another key's probe chain must not break lookups for
    // keys that come after it.
    Table t;
    initTable(&t);

    // Force several keys into the table so some are very likely to
    // collide/probe past each other in an 8-slot table.
    const char* keys[] = {"a", "b", "c", "d", "e", "f"};
    for (int i = 0; i < 6; i++) tableSet(&t, (char*)keys[i], NUMBER_VAL(i));

    ASSERT_TRUE(tableDelete(&t, "c"));

    // Every other key must still be reachable after the delete.
    Value out;
    for (int i = 0; i < 6; i++) {
        if (strcmp(keys[i], "c") == 0) continue;
        bool found = tableGet(&t, (char*)keys[i], &out);
        ASSERT_TRUE(found);
        ASSERT_TRUE(AS_NUMBER(out) == i);
    }

    // The deleted key is really gone.
    bool stillThere = tableGet(&t, "c", &out);
    ASSERT_TRUE(!stillThere);

    freeTable(&t);
}

TEST(delete_on_missing_key_returns_false) {
    Table t;
    initTable(&t);
    ASSERT_TRUE(!tableDelete(&t, "ghost"));
    freeTable(&t);
}

TEST(grows_past_load_factor_and_keeps_all_entries) {
    // capacity starts at 8, grows at 0.75 load factor — 50 inserts forces
    // several resizes. Every key must still resolve correctly afterward.
    Table t;
    initTable(&t);

    char keybuf[50][8];
    for (int i = 0; i < 50; i++) {
        snprintf(keybuf[i], sizeof(keybuf[i]), "k%d", i);
        tableSet(&t, keybuf[i], NUMBER_VAL(i));
    }

    ASSERT_EQ_INT(50, t.count);

    Value out;
    for (int i = 0; i < 50; i++) {
        bool found = tableGet(&t, keybuf[i], &out);
        ASSERT_TRUE(found);
        ASSERT_TRUE(AS_NUMBER(out) == i);
    }

    freeTable(&t);
}

TEST(hash_fnv_is_deterministic_and_position_sensitive) {
    uint32_t h1 = hashStringFNV("abc", 3);
    uint32_t h2 = hashStringFNV("abc", 3);
    uint32_t h3 = hashStringFNV("cba", 3);
    ASSERT_EQ_INT(h1, h2);
    ASSERT_TRUE(h1 != h3); // not a mathematical guarantee, but true for FNV-1a here
}

TEST(hashed_variants_match_plain_variants) {
    Table t;
    initTable(&t);

    uint32_t hash = hashStringFNV("shortcut", 8);
    tableSetHashed(&t, "shortcut", hash, BOOL_VAL(true));

    Value out;
    ASSERT_TRUE(tableGetHashed(&t, "shortcut", hash, &out));
    ASSERT_TRUE(IS_BOOL(out) && AS_BOOL(out) == true);

    // The plain (non-hashed) API must see the exact same entry.
    ASSERT_TRUE(tableGet(&t, "shortcut", &out));

    ASSERT_TRUE(tableDeleteHashed(&t, "shortcut", hash));
    ASSERT_TRUE(!tableGet(&t, "shortcut", &out));

    freeTable(&t);
}

// ── copyString()/interning tests (needs object.c + memory.c + VM stub) ──

TEST(copy_string_interns_equal_content_to_same_pointer) {
    resetTestVM();

    ObjString* a = copyString("hello", 5);
    ObjString* b = copyString("hello", 5);

    // Interning means equal content -> the SAME ObjString instance, not
    // just equal bytes. This is what lets table.c/object.c compare
    // ObjString keys by pointer instead of strcmp() everywhere.
    ASSERT_TRUE(a == b);
}

TEST(copy_string_distinguishes_different_content) {
    resetTestVM();

    ObjString* a = copyString("foo", 3);
    ObjString* b = copyString("bar", 3);
    ASSERT_TRUE(a != b);
}

TEST(table_find_string_locates_interned_string_by_raw_content) {
    resetTestVM();

    ObjString* s = copyString("needle", 6);
    // copyString() already interned it into vm.strings — tableFindString()
    // is what copyString() itself uses to detect the cache hit.
    ObjString* found = tableFindString(&vm.strings, "needle", 6, hashStringFNV("needle", 6));
    ASSERT_TRUE(found == s);
}

int main(void) {
    printf("table tests\n");
    RUN_TEST(set_and_get_roundtrip);
    RUN_TEST(get_on_missing_key_returns_false);
    RUN_TEST(set_on_existing_key_updates_value_not_count);
    RUN_TEST(delete_removes_key_but_keeps_probe_chain_intact);
    RUN_TEST(delete_on_missing_key_returns_false);
    RUN_TEST(grows_past_load_factor_and_keeps_all_entries);
    RUN_TEST(hash_fnv_is_deterministic_and_position_sensitive);
    RUN_TEST(hashed_variants_match_plain_variants);
    RUN_TEST(copy_string_interns_equal_content_to_same_pointer);
    RUN_TEST(copy_string_distinguishes_different_content);
    RUN_TEST(table_find_string_locates_interned_string_by_raw_content);
    TEST_SUMMARY();
}

// tests/test_memory.c
//
// Unit tests for src/memory.c: reallocate() (the single allocation
// choke-point + byte accounting) and the mark-and-sweep GC
// (markObject/collectGarbage).
//
// These need a real (test-double) VM — see tests/test_support.c — because
// reallocate() tracks vm.bytesAllocated/vm.nextGC, and collectGarbage()
// walks vm.current's fiber stack + vm.globals as roots.

#include "test.h"
#include "test_support.h"
#include "../src/memory.h"
#include "../src/object.h"
#include "../src/value.h"
#include "../src/table.h"
#include <string.h>

// ── reallocate(): allocation/byte-accounting ─────────────────────────────

TEST(reallocate_grows_and_tracks_bytes_allocated) {
    resetTestVM();
    ASSERT_EQ_INT(0, (long long)vm.bytesAllocated);

    char* buf = (char*)reallocate(NULL, 0, 16);
    ASSERT_TRUE(buf != NULL);
    ASSERT_EQ_INT(16, (long long)vm.bytesAllocated);

    buf = (char*)reallocate(buf, 16, 32);
    ASSERT_EQ_INT(32, (long long)vm.bytesAllocated);

    reallocate(buf, 32, 0); // free
    ASSERT_EQ_INT(0, (long long)vm.bytesAllocated);
}

TEST(reallocate_with_newsize_zero_frees_and_returns_null) {
    resetTestVM();
    char* buf = (char*)reallocate(NULL, 0, 8);
    void* result = reallocate(buf, 8, 0);
    ASSERT_TRUE(result == NULL);
}

TEST(reallocate_shrinking_reduces_tracked_bytes) {
    resetTestVM();
    char* buf = (char*)reallocate(NULL, 0, 100);
    ASSERT_EQ_INT(100, (long long)vm.bytesAllocated);
    buf = (char*)reallocate(buf, 100, 10);
    ASSERT_EQ_INT(10, (long long)vm.bytesAllocated);
    reallocate(buf, 10, 0);
}

// ── markObject() ──────────────────────────────────────────────────────────

TEST(mark_object_sets_marked_flag_and_queues_it_gray) {
    resetTestVM();
    ObjString* s = copyString("markme", 6);
    ASSERT_TRUE(s->obj.isMarked == false);

    markObject((Obj*)s);

    ASSERT_TRUE(s->obj.isMarked == true);
    ASSERT_EQ_INT(1, vm.grayCount);
    ASSERT_TRUE(vm.grayStack[0] == (Obj*)s);
}

TEST(mark_object_on_already_marked_object_is_a_no_op) {
    resetTestVM();
    ObjString* s = copyString("already", 7);
    markObject((Obj*)s);
    int grayCountAfterFirstMark = vm.grayCount;

    markObject((Obj*)s); // should not re-queue
    ASSERT_EQ_INT(grayCountAfterFirstMark, vm.grayCount);
}

TEST(mark_object_on_null_does_nothing) {
    resetTestVM();
    markObject(NULL); // must not crash
    ASSERT_EQ_INT(0, vm.grayCount);
}

// ── collectGarbage(): full mark-and-sweep cycle ──────────────────────────

TEST(collect_garbage_frees_unreachable_objects) {
    resetTestVM();

    // Nothing pushed to the test fiber's stack and nothing in globals,
    // so this string is unreachable the instant it's created.
    copyString("garbage", 7);
    size_t beforeCollect = vm.bytesAllocated;
    ASSERT_TRUE(beforeCollect > 0);

    collectGarbage();

    // An unreachable ObjString should have been swept: less memory is
    // live afterward than before the collection.
    ASSERT_TRUE(vm.bytesAllocated < beforeCollect);
}

TEST(collect_garbage_keeps_objects_reachable_from_the_fiber_stack) {
    resetTestVM();

    ObjString* kept = copyString("keepme", 6);
    testFiberPushRoot(OBJ_VAL(kept));

    collectGarbage();

    // Still marked-reachable, and readable — sweep() must not have freed it.
    ASSERT_TRUE(kept->length == 6);
    ASSERT_TRUE(strcmp(kept->chars, "keepme") == 0);
    // isMarked is reset to false by sweep() for every survivor, ready for
    // the next GC cycle.
    ASSERT_TRUE(kept->obj.isMarked == false);

    testFiberClearStack();
}

TEST(collect_garbage_keeps_objects_reachable_from_globals) {
    resetTestVM();

    ObjString* val = copyString("global-value", 12);
    tableSet(&vm.globals, "g", OBJ_VAL(val));

    collectGarbage();

    Value out;
    bool found = tableGet(&vm.globals, "g", &out);
    ASSERT_TRUE(found);
    ASSERT_TRUE(IS_OBJ(out) && AS_OBJ(out) == (Obj*)val);
}

TEST(collect_garbage_drops_dead_strings_from_the_intern_table) {
    // Regression test for the weak-table bug class: vm.strings must NOT
    // keep a string alive on its own, or nothing would ever be collected.
    resetTestVM();

    copyString("unreferenced", 12);
    ASSERT_TRUE(vm.strings.count >= 1);

    collectGarbage();

    Value dummy;
    bool stillInterned = tableGet(&vm.strings, "unreferenced", &dummy);
    ASSERT_TRUE(!stillInterned);
}

int main(void) {
    printf("memory tests\n");
    RUN_TEST(reallocate_grows_and_tracks_bytes_allocated);
    RUN_TEST(reallocate_with_newsize_zero_frees_and_returns_null);
    RUN_TEST(reallocate_shrinking_reduces_tracked_bytes);
    RUN_TEST(mark_object_sets_marked_flag_and_queues_it_gray);
    RUN_TEST(mark_object_on_already_marked_object_is_a_no_op);
    RUN_TEST(mark_object_on_null_does_nothing);
    RUN_TEST(collect_garbage_frees_unreachable_objects);
    RUN_TEST(collect_garbage_keeps_objects_reachable_from_the_fiber_stack);
    RUN_TEST(collect_garbage_keeps_objects_reachable_from_globals);
    RUN_TEST(collect_garbage_drops_dead_strings_from_the_intern_table);
    TEST_SUMMARY();
}

// tests/test_support.c
//
// table.c, memory.c, object.c, and compiler.c all assume a running VM
// exists (an `extern VM vm;` global, plus a handful of hooks normally
// implemented in src/tvm/vm_core.c: vmTrackObject, vmFindInternedString,
// vmInternString, tripCloseSocketHandle, vmGetScriptPath).
//
// src/tvm/vm_core.c itself can't be linked into a plain unit test binary
// standalone — it pulls in libcurl/OpenSSL initialization and the whole
// networking/fiber-scheduler stack, none of which is needed to test a
// hash table, an allocator, or a bytecode compiler.
//
// So this file provides a minimal, *faithful* stand-in: the three hook
// functions below are copied verbatim from vm_core.c's real
// implementation (they're one-liners), not reinvented — the goal is a
// real VM global with just enough wiring for table.c/memory.c/object.c/
// compiler.c to run exactly as they would inside the real interpreter,
// without needing curl/openssl/the scheduler to link.

#include "../src/vm.h"
#include "../src/object.h"
#include "../src/table.h"
#include "test_support.h"
#include <stdlib.h>

VM vm;

// ── Real vm_core.c hooks (verbatim) ──────────────────────────────────────
void vmTrackObject(Obj* obj) {
    obj->next = vm.objects;
    vm.objects = obj;
}

ObjString* vmFindInternedString(const char* chars, int length, uint32_t hash) {
    return tableFindString(&vm.strings, chars, length, hash);
}

void vmInternString(ObjString* string) {
    tableSet(&vm.strings, string->chars, OBJ_VAL(string));
}

// ── Test-only stand-ins (not exercised by table/memory/compiler tests) ──
// Real impl lives in src/tvm/net_tcp.c; no sockets exist in these tests.
void tripCloseSocketHandle(TripSocketHandle handle) {
    (void)handle;
}
// Real impl lives in src/tvm/db_mysql.c; no MySQL handles exist in these tests.
void tripMysqlCloseHandle(void* conn) {
    (void)conn;
}
// Stub for prepared-statement GC teardown (real impl in src/tvm/db_mysql.c).
// These test binaries link object.c without db_mysql.c, so the linker needs
// a no-op here — no MYSQL_STMT* ever exists in table/memory/compiler tests.
void tripMysqlCloseStmt(void* stmt) {
    (void)stmt;
}
// Stub for connection-pool GC teardown (real impl in src/tvm/db_mysql.c).
// Same reasoning as above — no MysqlPool* ever exists in these test binaries.
void tripMysqlFreePool(void* pool) {
    (void)pool;
}
// Real impl lives in src/tvm/vm_core.c via vmSetArgs(); no script is
// ever "run" by these tests, so there's no path to report.
const char* vmGetScriptPath(void) {
    return NULL;
}

// ── Test fixture setup/teardown ──────────────────────────────────────────
// Call at the start of each TEST() that touches the object/GC/global
// machinery, so one test's heap state can't leak into the next.
static Fiber testFiber;

void resetTestVM(void) {
    vm.chunk = NULL;
    vm.ip = NULL;

    testFiber.stack         = NULL;
    testFiber.stackCount    = 0;
    testFiber.stackCapacity = 0;
    testFiber.frames        = NULL;
    testFiber.frameCount    = 0;
    testFiber.frameCapacity = 0;
    testFiber.openUpvalues  = NULL;
    testFiber.tryHandlerCount = 0;
    testFiber.state    = FIBER_RUNNING;
    testFiber.next     = NULL;
    testFiber.waitFd   = TRIP_INVALID_SOCKET;

    vm.current   = &testFiber;
    vm.mainFiber = &testFiber;
    vm.readyHead = vm.readyTail = NULL;
    vm.blockedHead = vm.blockedTail = NULL;
    vm.anyFiberCrashed = false;

    initTable(&vm.globals);
    initTable(&vm.strings);
    vm.metaCount = 0;

    vm.objects = NULL;
    vm.bytesAllocated = 0;
    // Deliberately huge: table/compiler tests aren't exercising the GC and
    // shouldn't have a collection fire mid-test just because they allocated
    // a few strings. test_memory.c's GC-specific tests override this
    // themselves right before triggering a collection on purpose.
    vm.nextGC = (size_t)1 << 40;

    vm.grayStack    = NULL;
    vm.grayCount    = 0;
    vm.grayCapacity = 0;
}

// Gives a test a real (small) value stack on testFiber, so markRoots()
// has something concrete to walk. Returns the slot index the value was
// stored at, so the caller can also drop the reference later if it wants
// to test collection of that same object.
void testFiberPushRoot(Value v) {
    if (testFiber.stack == NULL) {
        testFiber.stack = (Value*)malloc(sizeof(Value) * 8);
        testFiber.stackCapacity = 8;
        testFiber.stackCount = 0;
    }
    testFiber.stack[testFiber.stackCount++] = v;
}

void testFiberClearStack(void) {
    testFiber.stackCount = 0;
}
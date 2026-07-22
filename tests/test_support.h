// tests/test_support.h
#ifndef TRIP_TEST_SUPPORT_H
#define TRIP_TEST_SUPPORT_H

#include "../src/value.h"
#include "../src/vm.h"

// tvm.h (not included here — it pulls in curl/openssl/pcre2) is normally
// where `extern VM vm;` lives. Test files that need direct access to the
// global test vm (e.g. to reach vm.strings) get the declaration from here
// instead.
extern VM vm;

// Resets the global test `vm` (defined in test_support.c) to a clean
// state: fresh empty globals/strings tables, empty object list, GC
// threshold set high enough that ordinary test allocations won't trigger
// a collection by accident. Call at the top of any TEST() that touches
// table.c/object.c/memory.c/compiler.c machinery.
void resetTestVM(void);

// Pushes `v` onto the test fiber's value stack, so collectGarbage()'s
// markRoots() has something concrete to find reachable. Use this to mark
// an object as "still alive" in a GC test.
void testFiberPushRoot(Value v);

// Empties the test fiber's value stack (so previously-pushed roots become
// unreachable on the next collectGarbage() call, unless referenced some
// other way).
void testFiberClearStack(void);

#endif

#ifndef vm_h
#define vm_h

#include <stddef.h>
#include "chunk.h"
#include "value.h"
#include "table.h"
#include "object.h"

// Tracks mutability for each global variable
typedef struct {
    char* key;
    bool isConst;
} VarMeta;

#define META_MAX 1024

typedef struct {
    ObjClosure* closure;
    uint8_t* ip;
    // NOTE: 'slots' is a STACK INDEX, not a raw pointer — because
    // the heap-allocated value stack may be realloc()'d to a new
    // address at any time (when the stack grows). Storing a raw
    // pointer into the old array would become a dangling pointer after
    // any realloc. We store an index instead, and convert it to a
    // pointer only at the point of use via FRAME_SLOTS(frame) macro.
    int slotsIndex;
} CallFrame;

// Records everything needed to jump back to a `catch` block (and optionally
// a `finally` block).
#define MAX_TRY_HANDLERS 32
typedef struct {
    int frameCount;
    int stackTopIndex;  // index, not pointer — same reason as slotsIndex above
    uint8_t* catchIp;
    // finally support — finallyIp is NULL when there is no finally block.
    uint8_t* finallyIp;
    bool     hasFinally;
} TryHandler;

// ── Fiber — one independent call stack ──────────────────────────────────
// Everything needed to suspend a running script and resume it later lives
// here: the value stack, the call-frame stack, active try/catch handlers,
// and open upvalues (which point into THIS fiber's stack, so they must
// travel with it rather than living on the shared VM).
//
// Today (Chapter A) there is exactly one Fiber — vm.current, aka the "main"
// fiber — so this is a straight field-for-field move out of VM with no
// behavior change. The scheduler (Chapter B) will add more Fibers linked
// via `next` and switch vm.current between them; the I/O reactor
// (Chapter C) will add `state`/`waitingOn` so a fiber blocked on
// tcpAccept()/tcpRead() can be parked without blocking the whole process.
typedef struct Fiber {
    // Growable value stack — heap-allocated, doubles in capacity whenever full
    Value*  stack;
    int     stackCount;     // number of values currently on the stack
    int     stackCapacity;  // total slots allocated

    // Growable call-frame array — same doubling strategy
    CallFrame* frames;
    int frameCount;
    int frameCapacity;

    ObjUpvalue* openUpvalues;

    TryHandler tryHandlers[MAX_TRY_HANDLERS];
    int tryHandlerCount;

    // ── Scheduler state ─────────────────────────────────────────────────
    enum { FIBER_READY, FIBER_RUNNING, FIBER_BLOCKED_IO, FIBER_DONE } state;
    struct Fiber* next;      // intrusive linked list — used for EITHER the
                              // ready queue OR the blocked-on-I/O queue,
                              // never both (a fiber is only ever in one).
    TripSocketHandle waitFd; // valid only when state == FIBER_BLOCKED_IO —
                              // the fd this fiber is waiting to become
                              // readable (or writable) on.
    bool waitForWrite;       // false = waiting to read, true = waiting to write
    long long waitDeadlineMs; // absolute monotonic deadline, or -1 = no timeout
    bool timedOut;           // set by the reactor if the deadline elapsed
                              // before the fd became ready — checked by the
                              // retried builtin instead of blindly retrying
                              // the syscall again
    void* httpPendingReq; 
} Fiber;

Fiber* newFiber(void);
void   freeFiber(Fiber* fiber);

typedef struct {
    Chunk* chunk;
    uint8_t* ip;

    Fiber* current;   // the fiber currently executing
    Fiber* mainFiber;

    // ── Chapter B: cooperative scheduler ────────────────────────────────
    // FIFO ready queue of fibers waiting for their turn, linked via
    // Fiber->next. vm.current is NOT in this queue while it's running —
    // it's added back (at the tail) only when it yields.
    Fiber* readyHead;
    Fiber* readyTail;

    // Fibers parked in tcpAccept()/tcpRead() waiting for their socket to
    // become readable. The reactor (pollBlockedFibers, in vm.c) moves them
    // to the ready queue once select() says they're ready.
    Fiber* blockedHead;
    Fiber* blockedTail;

    // Set when a spawned fiber hits an uncaught runtime error while other
    // fibers are still around to isolate it from. The crash doesn't take
    // the process down immediately, but the process should still exit
    // non-zero once everything finishes — a crash that nobody saw isn't
    // "fine", it's just deferred.
    bool anyFiberCrashed;

    Table globals;
    Table strings;   // interned strings — weak table, see tableRemoveWhite()
    VarMeta meta[META_MAX];
    int metaCount;

    Obj*        objects;       // intrusive linked list of ALL heap-allocated objects

    // ── GC state ─────────────────────────────────────────────────────────
    // How many bytes are currently live on the heap (tracked by reallocate).
    size_t bytesAllocated;
    // When bytesAllocated exceeds this threshold, trigger a collection.
    size_t nextGC;

    // Tri-color worklist: gray objects that have been marked reachable but
    // whose children haven't been traced yet.
    Obj**  grayStack;
    int    grayCount;
    int    grayCapacity;
} VM;

// Convert a frame's saved slot index back to a Value* for use in run().
// MUST be re-evaluated after every push() call (in case the stack was
// reallocated).
#define FRAME_SLOTS(frame) (vm.current->stack + (frame)->slotsIndex)

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_HANDLED_ERROR,
    // run() returns this when a fiber switch happened (spawn()'s target
    // fiber isn't started immediately, but yield() or a fiber finishing
    // both hand control to a different Fiber). The caller (runScheduler)
    // just calls run() again — it re-reads vm.current from scratch.
    INTERPRET_YIELD
} InterpretResult;

void initVM();
void freeVM();

InterpretResult interpret(const char* source);

void vmSetArgs(int argc, const char* const* argv, const char* scriptPath);
const char* vmGetScriptPath(void);

void push(Value value);
Value pop();

#endif
#include <stdlib.h>
#include <stdio.h>
#include "memory.h"
#include "vm.h"

// ── GC debug flag ─────────────────────────────────────────────────────────
// Set to 1 to print GC activity to stderr (useful during development).
#define GC_DEBUG 0

// Initial heap threshold before the first GC fires (1 MB).
#define GC_HEAP_GROW_FACTOR 2

// Declared in vm.c; we reference it here to access GC state.
extern VM vm;

// ── Forward declarations ───────────────────────────────────────────────────
static void markRoots();
static void traceReferences();
static void blackenObject(Obj* object);
static void sweep();

// ─────────────────────────────────────────────────────────────────────────
// reallocate — the single choke-point for all heap traffic.
//
// All allocation, reallocation, and freeing goes through here so we can:
//   1. Track vm.bytesAllocated accurately.
//   2. Trigger a collection when the heap grows past vm.nextGC.
// ─────────────────────────────────────────────────────────────────────────
void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
    // Update the live-byte counter.
    vm.bytesAllocated += newSize;
    vm.bytesAllocated -= oldSize;


    // Trigger GC when we've exceeded the threshold — but only on allocations,
    // not on frees (newSize > oldSize), to avoid collecting inside a free path.
    if (newSize > oldSize && vm.bytesAllocated > vm.nextGC) {
        collectGarbage();
    }

    // Free path: newSize == 0 means "free this pointer".
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }

    // Allocate / resize path.
    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "[trip] Out of memory — could not allocate %zu bytes\n", newSize);
        exit(1);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────
// markObject / markValue — Phase 1 helpers
//
// "Marking" means: set isMarked = true and push onto the gray worklist.
// The gray worklist lets us defer tracing children until traceReferences().
// ─────────────────────────────────────────────────────────────────────────
void markObject(Obj* object) {
    if (object == NULL) return;
    if (object->isMarked) return;   // already gray or black — skip

#if GC_DEBUG
    fprintf(stderr, "[GC] mark obj %p type=%d\n", (void*)object, object->type);
#endif

    object->isMarked = true;

    // Grow the gray worklist if needed.
    // We use raw realloc here (not our reallocate()) to avoid re-entering
    // the GC while we're already inside it.
    if (vm.grayCount >= vm.grayCapacity) {
        vm.grayCapacity = vm.grayCapacity < 8 ? 8 : vm.grayCapacity * 2;
        vm.grayStack = (Obj**)realloc(vm.grayStack, sizeof(Obj*) * vm.grayCapacity);
        if (vm.grayStack == NULL) {
            fprintf(stderr, "[trip] GC: out of memory growing gray stack\n");
            exit(1);
        }
    }
    vm.grayStack[vm.grayCount++] = object;
}

void markValue(Value value) {
    if (IS_OBJ(value)) markObject(AS_OBJ(value));
    // Primitive values (bool, number, char, nil) live on the stack — no heap object.
}

// ─────────────────────────────────────────────────────────────────────────
// markRoots — discover every GC root
//
// A root is any Value or Obj* that is directly reachable without tracing
// through another heap object.  Roots include:
//   • Every slot on the value stack
//   • Every frame's closure
//   • Every open upvalue
//   • Every entry in the globals table
//   • The compiler's chain of in-progress function objects (if compiling)
// ─────────────────────────────────────────────────────────────────────────
static void markTableValues(Table* table);     // forward

// Mark every root belonging to a single fiber: its value stack, its call
// frames' closures, and its open upvalues. Shared between vm.current and
// every fiber still sitting in the ready queue.
static void markFiberRoots(Fiber* fb) {
    if (!fb) return;

    // 1. Value stack — every live slot.
    for (int i = 0; i < fb->stackCount; i++) {
        markValue(fb->stack[i]);
    }

    // 2. Call frames — each frame holds a closure pointer.
    for (int i = 0; i < fb->frameCount; i++) {
        markObject((Obj*)fb->frames[i].closure);
    }

    // 3. Open upvalues — the linked list of upvalues still pointing into
    //    live stack slots.
    for (ObjUpvalue* uv = fb->openUpvalues; uv != NULL; uv = uv->next) {
        markObject((Obj*)uv);
    }
}

// Chapter B: a suspended fiber's stack is only reachable via the ready
// queue (vm.readyHead/next), NOT via vm.current — so it must be walked
// here explicitly, or a fiber parked mid-yield would look unreachable to
// the GC and get collected out from under it the moment it resumes.
static void markRoots() {
    if (!vm.current) return;  // defensive: GC must never fire before a fiber is current

    markFiberRoots(vm.current);
    for (Fiber* fb = vm.readyHead; fb != NULL; fb = fb->next) {
        markFiberRoots(fb);
    }
    // Fibers blocked on I/O (socket/HTTP) live only in vm.blockedHead until
    // pollBlockedFibers() moves them back to ready — they are just as much
    // a root as a ready fiber and must be walked here too, or a fiber
    // parked mid-await would look unreachable to the GC and get collected
    // out from under it while it waits.
    for (Fiber* fb = vm.blockedHead; fb != NULL; fb = fb->next) {
        markFiberRoots(fb);
    }

    // Global variables.
    markTableValues(&vm.globals);
}

// Mark every value stored in a Table (used for globals).
// Keys are ObjStrings and are also heap-allocated, but they live inside
// the Table's Entry array — mark them too.
static void markTableValues(Table* table) {
    // Table uses raw char* keys (not ObjString*) — only mark the Values.
    for (int i = 0; i < table->capacity; i++) {
        Entry* e = &table->entries[i];
        if (e->key == NULL) continue;
        markValue(e->value);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// blackenObject — trace a gray object's children
//
// "Blackening" means: the object itself is already marked; now mark every
// object it references so they enter the gray worklist.  After blackening,
// the object is "black" — both it and all its immediate children are marked.
// ─────────────────────────────────────────────────────────────────────────
static void blackenObject(Obj* object) {
#if GC_DEBUG
    fprintf(stderr, "[GC] blacken obj %p type=%d\n", (void*)object, object->type);
#endif

    switch (object->type) {
        case OBJ_STRING:
            // ObjString owns a char* buffer — not an Obj, no children to trace.
            break;

        case OBJ_UPVALUE: {
            ObjUpvalue* uv = (ObjUpvalue*)object;
            // When closed, the upvalue holds the value in uv->closed.
            // When open, uv->location points into the stack (already marked as a root).
            markValue(uv->closed);
            break;
        }

        case OBJ_FUNCTION: {
            ObjFunction* fn = (ObjFunction*)object;
            // The function's name is a heap string.
            markObject((Obj*)fn->name);
            // The constant pool can contain string or other object constants.
            for (int i = 0; i < fn->chunk.constants.count; i++) {
                markValue(fn->chunk.constants.values[i]);
            }
            // Default parameter values may reference heap objects.
            for (int i = 0; i < fn->defaultCount; i++) {
                markValue(fn->defaults[i]);
            }
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure* cl = (ObjClosure*)object;
            // The underlying function.
            markObject((Obj*)cl->function);
            // Each captured upvalue.
            for (int i = 0; i < cl->upvalueCount; i++) {
                markObject((Obj*)cl->upvalues[i]);
            }
            break;
        }

        case OBJ_LIST: {
            ObjList* list = (ObjList*)object;
            for (int i = 0; i < list->count; i++) {
                markValue(list->items[i]);
            }
            break;
        }

        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)object;
            for (int i = 0; i < dict->capacity; i++) {
                DictEntry* e = &dict->entries[i];
                // Skip both empty slots (NULL) and tombstone sentinels.
                if (!e->key || IS_DICT_TOMBSTONE(e)) continue;
                markObject((Obj*)e->key);
                markValue(e->value);
            }
            break;
        }

        case OBJ_SOCKET: {
            ObjSocket* sock = (ObjSocket*)object;
            // The native handle isn't an Obj, but serverSocket IS — it's a
            // back-pointer to the listening ObjSocket an accepted client
            // came from (see tcpAccept()/net_tcp.c). freeObject() and the
            // explicit-close path both dereference it (s->serverSocket->
            // activeConns--), so it must be a marked root or the listening
            // socket can be swept while a still-live client points at it.
            markObject((Obj*)sock->serverSocket);
            break;
        }

        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            markObject((Obj*)klass->name);
            markObject((Obj*)klass->methods);
            break;
        }

        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            markObject((Obj*)instance->klass);
            markObject((Obj*)instance->fields);
            break;
        }

        case OBJ_BOUND_METHOD: {
            ObjBoundMethod* bound = (ObjBoundMethod*)object;
            markValue(bound->receiver);
            markObject((Obj*)bound->method);
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// traceReferences — drain the gray worklist
//
// Pop gray objects one by one and blacken them.  blackenObject() may push
// more objects onto the gray stack, so we loop until it's empty.
// ─────────────────────────────────────────────────────────────────────────
static void traceReferences() {
    while (vm.grayCount > 0) {
        Obj* object = vm.grayStack[--vm.grayCount];
        blackenObject(object);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// sweep — walk vm.objects and free everything that wasn't marked
//
// After traceReferences(), every reachable object has isMarked == true.
// We walk the linked list, remove and free the unmarked ones, and clear
// the mark bit on the survivors so the next cycle starts clean.
// ─────────────────────────────────────────────────────────────────────────
static void sweep() {
    Obj* previous = NULL;
    Obj* object   = vm.objects;

    while (object != NULL) {
        if (object->isMarked) {
            // Survivor: clear the mark for the next cycle, keep in list.
            object->isMarked = false;
            previous = object;
            object   = object->next;
        } else {
            // Garbage: unlink and free.
            Obj* garbage = object;
            object = object->next;

            if (previous != NULL) {
                previous->next = object;
            } else {
                vm.objects = object;
            }

#if GC_DEBUG
            fprintf(stderr, "[GC] free obj %p type=%d\n", (void*)garbage, garbage->type);
#endif
            freeObject(garbage);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// collectGarbage — the public entry point
//
// Full tri-color mark-and-sweep:
//   1. Mark roots (turn white objects gray)
//   2. Trace references (turn gray objects black, propagating marks)
//   3. Sweep (free all remaining white objects)
//   4. Adjust the next-GC threshold
// ─────────────────────────────────────────────────────────────────────────
void collectGarbage() {
#if GC_DEBUG
    size_t before = vm.bytesAllocated;
    fprintf(stderr, "[GC] begin — %zu bytes live\n", before);
#endif

    // Phase 1 + 2: mark all reachable objects.
    markRoots();
    traceReferences();

    // vm.strings (the intern table) holds ObjStrings WEAKLY — it is never
    // walked by markRoots(), on purpose, or no string would ever die.
    // Any entry whose ObjString didn't get marked this cycle is about to
    // be freed by sweep(); drop it here first, or its char* key would
    // dangle the moment the object is gone.
    tableRemoveWhite(&vm.strings);

    // Phase 3: free unreachable objects.
    sweep();

    // Phase 4: grow the threshold so the next collection is triggered after
    // we've allocated roughly GC_HEAP_GROW_FACTOR × the current live set.
    vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
    // Never let the threshold fall below 1 MB (avoids thrashing on tiny programs).
    if (vm.nextGC < 1024 * 1024) vm.nextGC = 1024 * 1024;

#if GC_DEBUG
    fprintf(stderr, "[GC] end   — %zu bytes live (freed %zu), nextGC=%zu\n",
            vm.bytesAllocated,
            before > vm.bytesAllocated ? before - vm.bytesAllocated : 0,
            vm.nextGC);
#endif
}
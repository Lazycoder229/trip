#include "tvm.h"

VM vm;
int g_scriptArgc = 0;
const char* const* g_scriptArgv = NULL;
const char* g_scriptPath = NULL;

// ── Fiber Lifecycle ────────────────────────────────────────────────────────
Fiber* newFiber(void) {
    Fiber* f = (Fiber*)malloc(sizeof(Fiber));
    if (!f) { fprintf(stderr, "Fatal: out of memory allocating Fiber\n"); exit(1); }

    f->stack = (Value*)reallocate(NULL, 0, sizeof(Value) * 256); // STACK_INIT
    f->stackCount = 0;
    f->stackCapacity = 256;

    f->frames = (CallFrame*)reallocate(NULL, 0, sizeof(CallFrame) * 64); // FRAMES_INIT
    f->frameCount = 0;
    f->frameCapacity = 64;

    f->openUpvalues = NULL;
    f->tryHandlerCount = 0;

    f->state = FIBER_READY;
    f->next = NULL;
    f->waitFd = TRIP_INVALID_SOCKET;
    f->waitForWrite = false;
    f->waitDeadlineMs = -1;
    f->timedOut = false;
    f->httpPendingReq = NULL;
    return f;
}

void freeFiber(Fiber* f) {
    if (!f) return;
    reallocate(f->stack, sizeof(Value) * (size_t)f->stackCapacity, 0);
    f->stack = NULL;
    f->stackCount = f->stackCapacity = 0;
    reallocate(f->frames, sizeof(CallFrame) * (size_t)f->frameCapacity, 0);
    f->frames = NULL;
    f->frameCount = f->frameCapacity = 0;
    free(f);
}

// ── VM Lifecycle ───────────────────────────────────────────────────────────
void initVM(void) {
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;
    vm.grayStack    = NULL;
    vm.grayCount    = 0;
    vm.grayCapacity = 0;
    vm.objects = NULL;

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    
    // Initialize one-time OpenSSL state
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    vm.mainFiber = newFiber();
    vm.current   = vm.mainFiber;
    vm.readyHead = vm.readyTail = NULL;
    vm.blockedHead = vm.blockedTail = NULL;
    vm.anyFiberCrashed = false;

    initTable(&vm.globals);
    initTable(&vm.strings);
    vm.metaCount = 0;

    srand((unsigned int)time(NULL));

    tableSetHashed(&vm.globals, "PI", hashStringFNV("PI", 2), NUMBER_VAL(3.14159265358979323846));
    tableSetHashed(&vm.globals, "E",  hashStringFNV("E", 1),  NUMBER_VAL(2.71828182845904523536));
}

void freeVM(void) {
     httpMultiFree(); 
    freeFiber(vm.mainFiber);
    vm.mainFiber = NULL;
    vm.current = NULL;
    freeTable(&vm.globals);
    freeTable(&vm.strings);

    Obj* obj = vm.objects;
    while (obj) {
        Obj* next = obj->next;
        freeObject(obj);
        obj = next;
    }
    vm.objects = NULL;

    for (int i = 0; i < vm.metaCount; i++) {
        free(vm.meta[i].key);
        vm.meta[i].key = NULL;
    }
    vm.metaCount = 0;

    free(vm.grayStack);
    vm.grayStack    = NULL;
    vm.grayCount    = 0;
    vm.grayCapacity = 0;
}

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

// ── Main Entry ─────────────────────────────────────────────────────────────
InterpretResult interpret(const char* source) {
    resetStack();

    ObjFunction* script = newFunction(copyString("<script>", 8), 0);
    if (!compile(source, &script->chunk)) {
        return INTERPRET_COMPILE_ERROR;
    }

    ObjClosure* scriptClosure = newClosure(script);
    push((Value){VAL_OBJ, {.obj = (Obj*)scriptClosure}});

    if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
    vm.current->frames[0].closure = scriptClosure;
    vm.current->frames[0].ip = script->chunk.code;
    vm.current->frames[0].slotsIndex = 1;
    vm.current->frameCount = 1;

    vm.chunk = &script->chunk;
    vm.ip = vm.chunk->code;

    InterpretResult result;
    for (;;) {
        result = run();

        if (result == INTERPRET_YIELD) continue;

        if (result == INTERPRET_RUNTIME_ERROR) {
            vm.anyFiberCrashed = true;
            closeUpvalues(vm.current->stack + 0); // close all upvalues for this fiber
            Fiber* crashed = vm.current;
            crashed->state = FIBER_DONE;
            Fiber* next = nextFiberToRun();
            if (next == NULL) {
                return INTERPRET_RUNTIME_ERROR;
            }
            fprintf(stderr, "[trip] a fiber crashed — isolated; other fiber(s) keep running\n");
            next->state = FIBER_RUNNING;
            vm.current = next;
            if (crashed != vm.mainFiber) freeFiber(crashed);
            continue;
        }

        if (result == INTERPRET_OK && vm.anyFiberCrashed) {
            return INTERPRET_RUNTIME_ERROR;
        }
        return result;
    }
    return result;
}

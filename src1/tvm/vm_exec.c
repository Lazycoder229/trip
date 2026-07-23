#include "tvm.h"

InterpretResult finishCurrentFiber(void) {
    Fiber* finished = vm.current;
    finished->state = FIBER_DONE;
    Fiber* next = nextFiberToRun();
    if (next == NULL) {
        return INTERPRET_OK;
    }
    next->state = FIBER_RUNNING;
    vm.current = next;
    if (finished != vm.mainFiber) freeFiber(finished);
    return INTERPRET_YIELD;
}

InterpretResult run(void) {
    CallFrame* frame = &vm.current->frames[vm.current->frameCount - 1];

    #define READ_BYTE()     (*frame->ip++)
    #define READ_SHORT()    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
    #define READ_CONSTANT() (frame->closure->function->chunk.constants.values[READ_SHORT()])

   #define FILL_DEFAULTS(cl, ac) do {                                            \
    ObjFunction* _fn = (cl)->function;                                        \
    if (_fn->isVariadic) {                                                    \
        int _fixedArity = _fn->arity - 1;                                     \
        if ((ac) < _fixedArity) {                                             \
            InterpretResult _r = raiseError(                                  \
                "Expected at least %d argument(s) but got %d",                \
                _fixedArity, (ac));                                           \
            if (_r == INTERPRET_RUNTIME_ERROR) return _r;                     \
            frame = &vm.current->frames[vm.current->frameCount - 1];          \
            break;                                                            \
        }                                                                     \
        int _basePos = vm.current->stackCount - (ac);                        \
        int _extra   = (ac) - _fixedArity;                                   \
        ObjList* _list = newList();                                          \
        push((Value){VAL_OBJ, {.obj = (Obj*)_list}});                         \
        for (int _i = 0; _i < _extra; _i++) {                                \
            listAppend(_list, vm.current->stack[_basePos + _fixedArity + _i]);\
        }                                                                     \
        Value _listVal = pop();                                              \
        vm.current->stack[_basePos + _fixedArity] = _listVal;                \
        vm.current->stackCount = _basePos + _fixedArity + 1;                 \
        (ac) = (uint8_t)(_fixedArity + 1);                                   \
    } else if ((ac) < _fn->requiredArity || (ac) > _fn->arity) {             \
        InterpretResult _r;                                                   \
        if (_fn->requiredArity == _fn->arity) {                              \
            _r = raiseError("Expected %d arguments but got %d",              \
                            _fn->arity, (ac));                               \
        } else {                                                              \
            _r = raiseError("Expected %d to %d arguments but got %d",       \
                            _fn->requiredArity, _fn->arity, (ac));           \
        }                                                                     \
        if (_r == INTERPRET_RUNTIME_ERROR) return _r;                        \
        frame = &vm.current->frames[vm.current->frameCount - 1];             \
        break;                                                                \
    } else if ((ac) < _fn->arity) {                                          \
        int _first = (ac) - _fn->requiredArity;                             \
        int _miss  = _fn->arity - (ac);                                      \
        for (int _di = _first; _di < _first + _miss; _di++)                 \
            push(_fn->defaults[_di]);                                        \
        (ac) = (uint8_t)_fn->arity;                                         \
    }                                                                         \
} while (0)

    for (;;) {
        uint8_t instruction;
        switch (instruction = READ_BYTE()) {

            // ── Variables ──────────────────────────────────────────────────
            case OP_DEFINE_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                tableSetHashed(&vm.globals, name->chars, name->hash, peek(0)); pop(); break;
            }
            case OP_DEFINE_LET: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                tableSetHashed(&vm.globals, name->chars, name->hash, peek(0));
                registerVar(name->chars, false); pop(); break;
            }
            case OP_DEFINE_CONST: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                tableSetHashed(&vm.globals, name->chars, name->hash, peek(0));
                registerVar(name->chars, true); pop(); break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                if (isConst(name->chars)) { { InterpretResult _r = raiseError("Cannot reassign const '%s'", name->chars); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                Value existing;
                if (!tableGetHashed(&vm.globals, name->chars, name->hash, &existing)) { { InterpretResult _r = raiseError("Undefined variable '%s'", name->chars); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                tableSetHashed(&vm.globals, name->chars, name->hash, peek(0)); break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value value;
                if (!tableGetHashed(&vm.globals, name->chars, name->hash, &value)) { { InterpretResult _r = raiseError("Undefined variable '%s'", name->chars); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                push(value); break;
            }
            case OP_GET_LOCAL: { uint8_t slot = READ_BYTE(); push(FRAME_SLOTS(frame)[slot]); break; }
            case OP_SET_LOCAL: { uint8_t slot = READ_BYTE(); FRAME_SLOTS(frame)[slot] = peek(0); break; }

            // ── nil ────────────────────────────────────────────────────────
            case OP_NIL: push(NIL_VAL); break;

            // ── Comparison ─────────────────────────────────────────────────
            case OP_EQUAL:    { Value b=pop(),a=pop(); push(BOOL_VAL(valuesEqual(a,b))); break; }
            case OP_NOT_EQUAL:{ Value b=pop(),a=pop(); push(BOOL_VAL(!valuesEqual(a,b))); break; }
            case OP_LESS:     { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'<' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(BOOL_VAL(AS_NUMBER(a)<AS_NUMBER(b))); break; }
            case OP_LESS_EQUAL:{ Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'<=' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(BOOL_VAL(AS_NUMBER(a)<=AS_NUMBER(b))); break; }
            case OP_GREATER:  { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'>' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(BOOL_VAL(AS_NUMBER(a)>AS_NUMBER(b))); break; }
            case OP_GREATER_EQUAL:{ Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'>=' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(BOOL_VAL(AS_NUMBER(a)>=AS_NUMBER(b))); break; }
            case OP_NOT: { Value v=pop(); push(BOOL_VAL(isFalsy(v))); break; }

            // ── Control flow ───────────────────────────────────────────────
            case OP_POP: pop(); break;
            case OP_JUMP: { uint16_t offset=READ_SHORT(); frame->ip += offset; break; }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset=READ_SHORT();
                if (isFalsy(peek(0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: { uint16_t offset=READ_SHORT(); frame->ip -= offset; break; }

            // ── Constants ──────────────────────────────────────────────────
            case OP_CONSTANT: { push(READ_CONSTANT()); break; }

            // ── Arithmetic ─────────────────────────────────────────────────
            case OP_ADD: {
                Value b=pop(), a=pop();
                if (IS_NUMBER(a) && IS_NUMBER(b)) { push(NUMBER_VAL(AS_NUMBER(a)+AS_NUMBER(b))); }
                else if (IS_STRING(a) && IS_STRING(b)) {
                    push((Value){VAL_OBJ,{.obj=(Obj*)concatStrings(AS_STRING(a),AS_STRING(b))}});
                } else {
                    { InterpretResult _r = raiseError("'+' requires two numbers or two strings"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                break;
            }
            case OP_SUBTRACT: { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'-' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(NUMBER_VAL(AS_NUMBER(a)-AS_NUMBER(b))); break; }
            case OP_MULTIPLY: { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'*' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(NUMBER_VAL(AS_NUMBER(a)*AS_NUMBER(b))); break; }
            case OP_DIVIDE:   { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'/' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                if (AS_NUMBER(b)==0) { { InterpretResult _r = raiseError("Division by zero"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                push(NUMBER_VAL(AS_NUMBER(a)/AS_NUMBER(b))); break;
            }
            case OP_MODULO:   { Value b=pop(),a=pop();
                if (!IS_NUMBER(a)||!IS_NUMBER(b)) { InterpretResult _r=raiseError("'%%' requires two numbers"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(NUMBER_VAL(fmod(AS_NUMBER(a),AS_NUMBER(b)))); break; }
            case OP_NEGATE:   { Value v=pop();
                if (!IS_NUMBER(v)) { InterpretResult _r=raiseError("'-' (unary) requires a number"); if(_r==INTERPRET_RUNTIME_ERROR) return _r; frame=&vm.current->frames[vm.current->frameCount-1]; break; }
                push(NUMBER_VAL(-AS_NUMBER(v))); break; }

            // ── match ──────────────────────────────────────────────────────
            case OP_MATCH_EQUAL: {
                Value caseVal=pop(); Value subject=peek(0);
                push(BOOL_VAL(valuesEqual(subject, caseVal))); break;
            }

            // ── Collections ────────────────────────────────────────────────
            case OP_BUILD_LIST: {
                uint8_t count = READ_BYTE();
                ObjList* list = newList();
                #define BUILD_LIST_MAX 256
                _Static_assert(BUILD_LIST_MAX > 255,
                    "tmp[] in OP_BUILD_LIST must fit all uint8_t counts (0-255)");
                Value tmp[BUILD_LIST_MAX];
                #undef BUILD_LIST_MAX
                for (int i = count-1; i >= 0; i--) tmp[i] = pop();
                for (int i = 0; i < count; i++) listAppend(list, tmp[i]);
                push((Value){VAL_OBJ,{.obj=(Obj*)list}});
                break;
            }
            case OP_BUILD_DICT: {
                uint8_t pairs = READ_BYTE();
                Value tmp[512];
                for (int i = pairs*2-1; i >= 0; i--) tmp[i] = pop();
                bool dictKeyOk = true;
                for (int i = 0; i < pairs && dictKeyOk; i++) {
                    if (!IS_STRING(tmp[i*2])) dictKeyOk = false;
                }
                if (!dictKeyOk) {
                    InterpretResult _r = raiseError("Dict key must be string");
                    if (_r == INTERPRET_RUNTIME_ERROR) return _r;
                    frame = &vm.current->frames[vm.current->frameCount - 1];
                    break;
                }
                ObjDict* dict = newDict();
                for (int i = 0; i < pairs; i++) {
                    dictSet(dict, AS_STRING(tmp[i*2]), tmp[i*2+1]);
                }
                push((Value){VAL_OBJ,{.obj=(Obj*)dict}});
                break;
            }
            case OP_INDEX_GET: {
                Value idx = pop(); Value obj = pop();
                if (IS_LIST(obj)) {
                    ObjList* list = AS_LIST(obj);
                    if (!IS_NUMBER(idx)) { { InterpretResult _r = raiseError("List index must be number"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    int i = (int)AS_NUMBER(idx);
                    if (i < 0) i = list->count + i;
                    if (i < 0 || i >= list->count) { { InterpretResult _r = raiseError("List index out of range"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    push(list->items[i]);
                } else if (IS_DICT(obj)) {
                    if (!IS_STRING(idx)) { { InterpretResult _r = raiseError("Dict key must be string"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    Value out;
                    if (!dictGet(AS_DICT(obj), AS_STRING(idx), &out)) push(NIL_VAL);
                    else push(out);
                } else if (IS_STRING(obj)) {
                    ObjString* s = AS_STRING(obj);
                    if (!IS_NUMBER(idx)) { { InterpretResult _r = raiseError("String index must be number"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    int i = (int)AS_NUMBER(idx);
                    if (i < 0) i = s->length + i;
                    if (i < 0 || i >= s->length) { { InterpretResult _r = raiseError("String index out of range"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    push(CHAR_VAL(s->chars[i]));
                } else {
                    { InterpretResult _r = raiseError("Value of type '%s' is not indexable", typeName(obj)); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                break;
            }
            case OP_INDEX_SET: {
                Value val = pop(); Value idx = pop(); Value obj = pop();
                if (IS_LIST(obj)) {
                    ObjList* list = AS_LIST(obj);
                    if (!IS_NUMBER(idx)) { { InterpretResult _r = raiseError("List index must be number"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    int i = (int)AS_NUMBER(idx);
                    if (i < 0) i = list->count + i;
                    if (i < 0 || i >= list->count) { { InterpretResult _r = raiseError("List index out of range"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    list->items[i] = val;
                } else if (IS_DICT(obj)) {
                    if (!IS_STRING(idx)) { { InterpretResult _r = raiseError("Dict key must be string"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; } }
                    dictSet(AS_DICT(obj), AS_STRING(idx), val);
                } else {
                    { InterpretResult _r = raiseError("Value of type '%s' does not support index assignment", typeName(obj)); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                push(val);
                break;
            }

            // ── Built-ins & Methods ────────────────────────────────────────
            case OP_CALL: {
                uint8_t argc = READ_BYTE();
                Value callee = peek(argc);
                if (IS_CLASS(callee)) {
                    ObjClass* klass = AS_CLASS(callee);
                    ObjInstance* instance = newInstance(klass);
                    vm.current->stack[vm.current->stackCount - argc - 1] = (Value){VAL_OBJ, {.obj = (Obj*)instance}};
                    
                    Value initMethod;
                    ObjString* initStr = copyString("init", 4);
                    if (dictGet(klass->methods, initStr, &initMethod)) {
                        ObjClosure* closure = AS_CLOSURE(initMethod);
                        FILL_DEFAULTS(closure, argc);
                        if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                        CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                        newFrame->closure = closure;
                        newFrame->ip = closure->function->chunk.code;
                        newFrame->slotsIndex = vm.current->stackCount - argc - 1;
                        frame = newFrame;
                    } else if (argc != 0) {
                        { InterpretResult _r = raiseError("Expected 0 arguments but got %d", argc); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                    }
                    break;
                }

                if (IS_BOUND_METHOD(callee)) {
                    ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                    vm.current->stack[vm.current->stackCount - argc - 1] = bound->receiver;
                    callee = (Value){VAL_OBJ, {.obj = (Obj*)bound->method}};
                }

                if (!IS_CLOSURE(callee)) {
                    { InterpretResult _r = raiseError("Can only call functions and classes"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                ObjClosure* closure = AS_CLOSURE(callee);
                FILL_DEFAULTS(closure, argc);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure = closure;
                newFrame->ip = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - argc - 1;
                frame = newFrame;
                break;
            }
            case OP_RETURN_VAL: {
                Value result = pop();
                CallFrame* caller = frame;
                closeUpvalues(vm.current->stack + caller->slotsIndex);
                vm.current->frameCount--;
                if (vm.current->frameCount == 0) {
                    pop();
                    return finishCurrentFiber();
                }
                vm.current->stackCount = caller->slotsIndex;
                frame = &vm.current->frames[vm.current->frameCount - 1];
                push(result);
                break;
            }
            case OP_RETURN_NIL: {
                CallFrame* caller = frame;
                closeUpvalues(vm.current->stack + caller->slotsIndex);
                vm.current->frameCount--;
                if (vm.current->frameCount == 0) {
                    pop();
                    return finishCurrentFiber();
                }
                vm.current->stackCount = caller->slotsIndex;
                frame = &vm.current->frames[vm.current->frameCount - 1];
                push(NIL_VAL);
                break;
            }
            case OP_CLOSURE: {
                ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure* closure = newClosure(function);
                push((Value){VAL_OBJ, {.obj = (Obj*)closure}});
                for (int i = 0; i < function->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index   = READ_BYTE();
                    if (isLocal) {
                        closure->upvalues[i] = captureUpvalue(vm.current->stack + frame->slotsIndex + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }
            case OP_CLOSE_UPVALUE: {
                closeUpvalues(vm.current->stack + vm.current->stackCount - 1);
                pop();
                break;
            }
            case OP_CALL_BUILTIN: {
                uint8_t id   = READ_BYTE();
                uint8_t argc = READ_BYTE();
                InterpretResult r = callBuiltin(id, argc);
                if (r == INTERPRET_RUNTIME_ERROR || r == INTERPRET_YIELD) return r;
                if (r == INTERPRET_HANDLED_ERROR) frame = &vm.current->frames[vm.current->frameCount - 1];
                break;
            }
            case OP_CALL_METHOD: {
                uint8_t id   = READ_BYTE();
                uint8_t argc = READ_BYTE();
                InterpretResult r = callMethod(id, argc);
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                if (r == INTERPRET_HANDLED_ERROR) frame = &vm.current->frames[vm.current->frameCount - 1];
                break;
            }
            case OP_CLASS: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                push((Value){VAL_OBJ, {.obj = (Obj*)newClass(name)}});
                break;
            }
            case OP_METHOD: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value method = peek(0);
                ObjClass* klass = AS_CLASS(peek(1));
                dictSet(klass->methods, name, method);
                pop();
                break;
            }
            case OP_PROPERTY_GET: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value obj = peek(0);
                if (!IS_INSTANCE(obj)) {
                    { InterpretResult _r = raiseError("Only instances have properties"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                ObjInstance* instance = AS_INSTANCE(obj);
                Value value;
                if (dictGet(instance->fields, name, &value)) {
                    pop(); // instance
                    push(value);
                    break;
                }
                if (dictGet(instance->klass->methods, name, &value)) {
                    ObjBoundMethod* bound = newBoundMethod(obj, AS_CLOSURE(value));
                    pop(); // instance
                    push((Value){VAL_OBJ, {.obj = (Obj*)bound}});
                    break;
                }
                { InterpretResult _r = raiseError("Undefined property '%s'", name->chars); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
            }
            case OP_PROPERTY_SET: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value obj = peek(1);
                if (!IS_INSTANCE(obj)) {
                    { InterpretResult _r = raiseError("Only instances have properties"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                ObjInstance* instance = AS_INSTANCE(obj);
                dictSet(instance->fields, name, peek(0));
                Value val = pop(); pop(); // val, instance
                push(val);
                break;
            }
            case OP_INVOKE: {
                uint16_t methodConst = READ_SHORT();
                ObjString* methodName = AS_STRING(
                    frame->closure->function->chunk.constants.values[methodConst]);
                uint8_t argc = READ_BYTE();
                Value receiver = peek(argc);

                if (!IS_INSTANCE(receiver)) {
                    { InterpretResult _r = raiseError("Only instances have methods"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }
                ObjInstance* instance = AS_INSTANCE(receiver);
                
                Value fieldVal;
                if (dictGet(instance->fields, methodName, &fieldVal)) {
                    vm.current->stack[vm.current->stackCount - argc - 1] = fieldVal;
                    if (!IS_CLOSURE(fieldVal)) {
                        { InterpretResult _r = raiseError("Can only call functions"); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                    }
                    ObjClosure* closure = AS_CLOSURE(fieldVal);
                    FILL_DEFAULTS(closure, argc);
                    if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                    CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                    newFrame->closure = closure;
                    newFrame->ip = closure->function->chunk.code;
                    newFrame->slotsIndex = vm.current->stackCount - argc - 1;
                    frame = newFrame;
                    break;
                }

                Value methodVal;
                if (!dictGet(instance->klass->methods, methodName, &methodVal)) {
                    { InterpretResult _r = raiseError("Undefined property '%s'", methodName->chars); if (_r == INTERPRET_RUNTIME_ERROR) return _r; frame = &vm.current->frames[vm.current->frameCount - 1]; break; }
                }

                ObjClosure* closure = AS_CLOSURE(methodVal);
                FILL_DEFAULTS(closure, argc);
                if (vm.current->frameCount >= vm.current->frameCapacity) growFrames();
                CallFrame* newFrame = &vm.current->frames[vm.current->frameCount++];
                newFrame->closure = closure;
                newFrame->ip = closure->function->chunk.code;
                newFrame->slotsIndex = vm.current->stackCount - argc - 1;
                frame = newFrame;
                break;
            }

            // ── try / catch / throw ─────────────────────────────────────────
            case OP_TRY_BEGIN: {
                uint16_t catchOffset   = READ_SHORT();
                uint16_t finallyOffset = READ_SHORT();
                if (vm.current->tryHandlerCount >= MAX_TRY_HANDLERS) {
                    InterpretResult _r = raiseError("Too many nested try blocks");
                    if (_r == INTERPRET_RUNTIME_ERROR) return _r;
                    frame = &vm.current->frames[vm.current->frameCount - 1];
                    break;
                }
                TryHandler* h = &vm.current->tryHandlers[vm.current->tryHandlerCount++];
                h->frameCount    = vm.current->frameCount;
                h->stackTopIndex = vm.current->stackCount;
                h->catchIp = frame->ip + catchOffset;
                if (finallyOffset == 0xFFFF) {
                    h->hasFinally  = false;
                    h->finallyIp   = NULL;
                } else {
                    h->hasFinally  = true;
                    h->finallyIp   = frame->ip + finallyOffset;
                }
                break;
            }
            case OP_TRY_END: {
                vm.current->tryHandlerCount--;
                break;
            }
            case OP_THROW: {
                Value errVal = pop();
                InterpretResult r = raiseValue(errVal);
                if (r == INTERPRET_RUNTIME_ERROR) return r;
                frame = &vm.current->frames[vm.current->frameCount - 1];
                break;
            }

            // ── Finally ───────────────────────────────────────────────────
            case OP_FINALLY_BEGIN:
                break;

            case OP_FINALLY_END: {
                Value pending = pop();
                if (!IS_NIL(pending)) {
                    InterpretResult r = raiseValue(pending);
                    if (r == INTERPRET_RUNTIME_ERROR) return r;
                    frame = &vm.current->frames[vm.current->frameCount - 1];
                }
                break;
            }

            // ── I/O ────────────────────────────────────────────────────────
            case OP_PRINT:    { printValue(pop()); break; }
            case OP_PRINT_NL: { printValue(pop()); putchar('\n'); break; }
            case OP_RETURN: {
                // Only emitted once, as the implicit terminator of the
                // top-level script chunk. Close any upvalues still open
                // from this frame for consistency with OP_RETURN_VAL /
                // OP_RETURN_NIL, which both do this before finishing.
                closeUpvalues(vm.current->stack + 0);
                return finishCurrentFiber();
            }
        }
    }

    #undef READ_BYTE
    #undef READ_SHORT
    #undef READ_CONSTANT
}
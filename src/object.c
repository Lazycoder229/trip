#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "object.h"
#include <stdint.h>
#include "table.h"
#include "memory.h"

// Called by allocateObject() for every new heap object — threads it into
// the VM's GC linked list. Defined in vm.c; declared here rather than via
// vm.h to avoid the object.h <-> vm.h circular dependency.
void vmTrackObject(Obj* obj);

// String interning hooks — defined in vm.c (which owns vm.strings),
// declared here for the same reason as vmTrackObject() above.
ObjString* vmFindInternedString(const char* chars, int length, uint32_t hash);
void       vmInternString(ObjString* string);

// ── allocate any Obj subtype ──────────────────────────────────────────────
static Obj* allocateObject(size_t size, ObjType type) {
    Obj* object = (Obj*)reallocate(NULL, 0, size);
    object->type     = type;
    object->isMarked = false;   // starts white — the GC will mark it if reachable
    object->next     = NULL;
    // Thread into the VM's GC list so freeVM() can release every heap object.
    vmTrackObject(object);
    return object;
}

// ── String ────────────────────────────────────────────────────────────────
ObjString* copyString(const char* chars, int length) {
    uint32_t hash = hashStringFNV(chars, length);

    // Already interned? Return the canonical ObjString — no new allocation.
    ObjString* interned = vmFindInternedString(chars, length, hash);
    if (interned != NULL) return interned;

    char* heapChars = (char*)reallocate(NULL, 0, (size_t)(length + 1));
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';

    ObjString* string = (ObjString*)allocateObject(sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->chars  = heapChars;
    string->hash   = hash;

    vmInternString(string);
    return string;
}

// ── List ──────────────────────────────────────────────────────────────────
ObjList* newList() {
    ObjList* list = (ObjList*)allocateObject(sizeof(ObjList), OBJ_LIST);
    list->count    = 0;
    list->capacity = 0;
    list->items    = NULL;
    return list;
}

void listAppend(ObjList* list, Value value) {
    if (list->capacity < list->count + 1) {
        int oldCap     = list->capacity;
        int newCap     = oldCap < 8 ? 8 : oldCap * 2;
        list->items    = (Value*)reallocate(list->items,
                                            sizeof(Value) * (size_t)oldCap,
                                            sizeof(Value) * (size_t)newCap);
        list->capacity = newCap;
    }
    list->items[list->count++] = value;
}

void listFree(ObjList* list) {
    reallocate(list->items, sizeof(Value) * (size_t)list->capacity, 0);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

// ── Dict (open-addressing, load factor 0.75) ──────────────────────────────
//
// Tombstone sentinel — defined here (storage), declared extern in object.h.
// Zero-initialised by the C runtime; never used as a real string.
// See object.h for DICT_TOMBSTONE / IS_DICT_TOMBSTONE macros.
ObjString DICT_TOMBSTONE_OBJ;

static DictEntry* findEntry(DictEntry* entries, int capacity, ObjString* key) {
    // Reuse the hash cached on the ObjString at creation time (copyString())
    // instead of recomputing it on every dict access.
    uint32_t idx = key->hash & (uint32_t)(capacity - 1);
    DictEntry* tombstone = NULL;
    for (;;) {
        DictEntry* e = &entries[idx];
        if (e->key == NULL) {
            // Truly empty slot — end of probe chain.
            // Return tombstone slot if we passed one (allows reuse).
            return tombstone ? tombstone : e;
        }
        if (IS_DICT_TOMBSTONE(e)) {
            // Deleted slot — remember it for possible reuse, keep probing.
            if (!tombstone) tombstone = e;
        } else if (e->key == key) {
            // Pointer compare — safe because copyString() is the single
            // choke point for ObjString creation and interns every string,
            // so equal content always means equal pointer.
            return e;   // found it
        }
        idx = (idx + 1) & (uint32_t)(capacity - 1);
    }
}

static void dictGrow(ObjDict* dict) {
    int newCap = dict->capacity < 8 ? 8 : dict->capacity * 2;
    DictEntry* newEntries = (DictEntry*)reallocate(NULL, 0,
                                sizeof(DictEntry) * (size_t)newCap);
    // Zero-initialise so all keys start NULL (truly empty).
    for (int i = 0; i < newCap; i++) newEntries[i].key = NULL;
    // Re-insert live entries only — tombstones are dropped on resize.
    int newCount = 0;
    for (int i = 0; i < dict->capacity; i++) {
        DictEntry* src = &dict->entries[i];
        if (!src->key || IS_DICT_TOMBSTONE(src)) continue;
        DictEntry* dst = findEntry(newEntries, newCap, src->key);
        *dst = *src;
        newCount++;
    }
    reallocate(dict->entries, sizeof(DictEntry) * (size_t)dict->capacity, 0);
    dict->entries  = newEntries;
    dict->capacity = newCap;
    dict->count    = newCount;   // tombstones are gone; update live count
}

ObjDict* newDict() {
    ObjDict* dict = (ObjDict*)allocateObject(sizeof(ObjDict), OBJ_DICT);
    dict->count    = 0;
    dict->capacity = 0;
    dict->entries  = NULL;
    return dict;
}

ObjFunction* newFunction(ObjString* name, int arity) {
    ObjFunction* function = (ObjFunction*)allocateObject(sizeof(ObjFunction), OBJ_FUNCTION);
    function->arity         = arity;
    function->requiredArity = arity;   // updated later if defaults are added
    function->defaultCount  = 0;
    function->defaults      = NULL;
    function->upvalueCount  = 0;
     function->isVariadic    = false; 
    function->name          = name;
    initChunk(&function->chunk);
    return function;
}

// ── Closure ──────────────────────────────────────────────────────────────
ObjClosure* newClosure(ObjFunction* function) {
    // Allocate at least 1 slot even when there are no upvalues — this avoids
    // a reallocate(NULL, 0, 0) which some allocators treat as a no-op and
    // return NULL, making the upvalues pointer non-NULL-but-zero-sized.
    // IMPORTANT: store uvCount (the actual allocation size) in upvalueCount
    // so that freeObject() passes the correct oldSize to reallocate().
    // Using function->upvalueCount (which can be 0) would cause a size
    // mismatch: 1 slot allocated but 0 bytes reported freed, permanently
    // inflating vm.bytesAllocated and triggering premature GC.
    int uvCount = function->upvalueCount > 0 ? function->upvalueCount : 1;
    ObjUpvalue** upvalues = (ObjUpvalue**)reallocate(NULL, 0, sizeof(ObjUpvalue*) * (size_t)uvCount);
    for (int i = 0; i < uvCount; i++) upvalues[i] = NULL;

    ObjClosure* closure = (ObjClosure*)allocateObject(sizeof(ObjClosure), OBJ_CLOSURE);
    closure->function     = function;
    closure->upvalues     = upvalues;
    closure->upvalueCount = uvCount;  // matches what was actually allocated
    return closure;
}

// ── Upvalue ──────────────────────────────────────────────────────────────
ObjUpvalue* newUpvalue(Value* slot) {
    ObjUpvalue* upvalue = (ObjUpvalue*)allocateObject(sizeof(ObjUpvalue), OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = (Value){VAL_NIL, {.number = 0}};
    upvalue->next = NULL;
    return upvalue;
}

// ── Socket ───────────────────────────────────────────────────────────────
ObjSocket* newSocket(TripSocketHandle handle, bool isListening) {
    ObjSocket* socket = (ObjSocket*)allocateObject(sizeof(ObjSocket), OBJ_SOCKET);
    socket->handle       = handle;
    socket->isListening  = isListening;
    socket->closed       = false;
    socket->maxConns     = -1;    // -1 = unlimited
    socket->activeConns  = 0;
    socket->serverSocket = NULL;  // set for accepted client sockets
    // TLS fields — MUST be zeroed here. allocateObject() goes through
    // reallocate() -> realloc(), which does NOT zero fresh memory, so
    // leaving these uninitialized means every plain TCP socket (and every
    // TLS listening socket, which legitimately has no SSL* of its own)
    // carries garbage in ssl/ctx. tlsClose()/freeObject() both guard
    // their SSL cleanup behind `if (s->ssl != NULL)` — with garbage instead
    // of a real NULL, that check passes by accident and SSL_shutdown()/
    // SSL_free() get called on a bogus pointer, crashing.
    socket->ssl              = NULL;
    socket->ctx              = NULL;
    socket->ctxOwned         = false;
    socket->tlsHandshakeDone = false;
    return socket;
}

ObjClass* newClass(ObjString* name) {
    ObjClass* klass = (ObjClass*)allocateObject(sizeof(ObjClass), OBJ_CLASS);
    klass->name = name;
    klass->methods = newDict();
    return klass;
}

ObjInstance* newInstance(ObjClass* klass) {
    ObjInstance* instance = (ObjInstance*)allocateObject(sizeof(ObjInstance), OBJ_INSTANCE);
    instance->klass = klass;
    instance->fields = newDict();
    return instance;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
    ObjBoundMethod* bound = (ObjBoundMethod*)allocateObject(sizeof(ObjBoundMethod), OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;
    return bound;
}

void dictSet(ObjDict* dict, ObjString* key, Value value) {
    if (dict->count + 1 > dict->capacity * 3 / 4) dictGrow(dict);
    DictEntry* e = findEntry(dict->entries, dict->capacity, key);
    // Increment count only for genuinely new slots (not tombstone reuse or updates).
    if (!e->key) dict->count++;          // truly empty slot
    // Tombstone slots are reused without incrementing count (they were
    // already counted when the entry was live).
    e->key   = key;
    e->value = value;
}

bool dictGet(ObjDict* dict, ObjString* key, Value* out) {
    if (!dict->capacity) return false;
    DictEntry* e = findEntry(dict->entries, dict->capacity, key);
    if (!e->key || IS_DICT_TOMBSTONE(e)) return false;
    *out = e->value;
    return true;
}

bool dictDelete(ObjDict* dict, ObjString* key) {
    if (!dict->capacity) return false;
    DictEntry* e = findEntry(dict->entries, dict->capacity, key);
    if (!e->key || IS_DICT_TOMBSTONE(e)) return false;
    // Mark as tombstone (not NULL) so the probe chain stays intact for
    // any entries that hash-collided past this slot.
    e->key = DICT_TOMBSTONE;
    dict->count--;
    return true;
}

void dictFree(ObjDict* dict) {
    reallocate(dict->entries, sizeof(DictEntry) * (size_t)dict->capacity, 0);
    dict->entries  = NULL;
    dict->count    = 0;
    dict->capacity = 0;
}

// ── freeObject — release the resources owned by a single heap object ──────
// Called by freeVM() when walking the vm.objects list. Each subtype has
// its own heap members that need freeing in addition to the Obj header
// itself (which is freed by the caller via free(object)).
void freeObject(Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* s = (ObjString*)object;
            // Free the character buffer through reallocate() so bytesAllocated stays accurate.
            reallocate(s->chars, (size_t)(s->length + 1), 0);
            reallocate(object, sizeof(ObjString), 0);
            break;
        }
        case OBJ_LIST: {
            ObjList* l = (ObjList*)object;
            reallocate(l->items, sizeof(Value) * (size_t)l->capacity, 0);
            reallocate(object, sizeof(ObjList), 0);
            break;
        }
        case OBJ_DICT: {
            ObjDict* d = (ObjDict*)object;
            reallocate(d->entries, sizeof(DictEntry) * (size_t)d->capacity, 0);
            reallocate(object, sizeof(ObjDict), 0);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* fn = (ObjFunction*)object;
            freeChunk(&fn->chunk);
            if (fn->defaults != NULL) {
                reallocate(fn->defaults, sizeof(Value) * (size_t)fn->defaultCount, 0);
            }
            // fn->name is tracked by the GC list itself — don't free here
            reallocate(object, sizeof(ObjFunction), 0);
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure* cl = (ObjClosure*)object;
            reallocate(cl->upvalues, sizeof(ObjUpvalue*) * (size_t)cl->upvalueCount, 0);
            // cl->function is tracked by the GC list itself
            reallocate(object, sizeof(ObjClosure), 0);
            break;
        }
        case OBJ_UPVALUE:
            // ObjUpvalue owns no heap members beyond the Obj header
            reallocate(object, sizeof(ObjUpvalue), 0);
            break;
        case OBJ_SOCKET: {
            ObjSocket* sock = (ObjSocket*)object;
            // Safety net: if the script never called tcpClose(), close the
            // native handle here so the GC doesn't leak OS socket handles.
            // The real close path is BUILTIN_TCP_CLOSE in vm.c; this only
            // fires for sockets that became unreachable without an explicit close.
            if (!sock->closed) {
                tripCloseSocketHandle(sock->handle);
                sock->closed = true;
                // Fix 1: release the slot on the parent server if this was an
                // accepted client socket that was GC'd without an explicit close.
                if (sock->serverSocket != NULL && !sock->serverSocket->closed) {
                    sock->serverSocket->activeConns--;
                }
            }
            reallocate(object, sizeof(ObjSocket), 0);
            break;
        }
        case OBJ_CLASS: {
            // name and methods are GC managed
            reallocate(object, sizeof(ObjClass), 0);
            break;
        }
        case OBJ_INSTANCE: {
            // klass and fields are GC managed
            reallocate(object, sizeof(ObjInstance), 0);
            break;
        }
        case OBJ_BOUND_METHOD: {
            // receiver and method are GC managed
            reallocate(object, sizeof(ObjBoundMethod), 0);
            break;
        }
    }
    // Note: each case above already frees the Obj header via reallocate().
    // Do NOT call free(object) here.
}
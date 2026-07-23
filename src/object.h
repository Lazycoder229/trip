#ifndef object_h
#define object_h

#include <stdbool.h>
#include <stdint.h>
#include "value.h"
#include "chunk.h"
#include <openssl/ssl.h>

// ── Obj types ────────────────────────────────────────────────────────────
typedef enum {
    OBJ_STRING,
    OBJ_LIST,
    OBJ_DICT,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_SOCKET,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD,
    OBJ_DB_CONN,
} ObjType;

// Base heap object header (all heap objects start with this)
struct Obj {
    ObjType      type;
    bool         isMarked;   // GC: true = reachable (black/gray), false = white
    struct Obj*  next;       // intrusive linked list for GC
};

// ── String ───────────────────────────────────────────────────────────────
typedef struct ObjString {
    Obj  obj;
    int  length;
    char* chars;
    uint32_t hash;   // computed once in copyString(), reused everywhere
} ObjString;

// ── List  [ v0, v1, v2, ... ] ────────────────────────────────────────────
typedef struct ObjList {
    Obj        obj;
    int        count;
    int        capacity;
    Value*     items;     // dynamic array of Value
} ObjList;

// ── Dict  { key: value, ... }  (simple open-addressing) ──────────────────
typedef struct {
    ObjString* key;       // NULL = empty slot; DICT_TOMBSTONE = deleted slot
    Value      value;
} DictEntry;

// Tombstone sentinel used by dictDelete().
// Using NULL for deleted entries breaks open-addressing probe chains;
// this non-NULL sentinel lets findEntry() skip deleted slots while
// still stopping the probe at a truly empty (NULL) slot.
// Declared extern so memory.c (GC tracing) can test for it.
extern struct ObjString DICT_TOMBSTONE_OBJ;
#define DICT_TOMBSTONE      (&DICT_TOMBSTONE_OBJ)
#define IS_DICT_TOMBSTONE(e) ((e)->key == DICT_TOMBSTONE)

typedef struct ObjDict {
    Obj       obj;
    int       count;      // live entries
    int       capacity;   // allocated slots
    DictEntry* entries;
} ObjDict;

typedef struct ObjFunction {
    Obj       obj;
    int       arity;          // total parameter count (required + optional)
    int       requiredArity;  // parameters WITHOUT a default (must be supplied)
    int       upvalueCount;   // how many variables this function closes over
    Chunk     chunk;
    ObjString* name;
    // Default values for optional parameters.
    // defaults[0] is the default for params[requiredArity],
    // defaults[1] for params[requiredArity + 1], etc.
    // NULL when there are no defaults (requiredArity == arity).
    Value*    defaults;
    int       defaultCount;   // == arity - requiredArity
     bool      isVariadic;
} ObjFunction;

// ── Upvalue — a heap "box" a closure uses to reach a variable that lives
// in an enclosing function's stack frame (or, once that frame is gone,
// a copy of the value it held). ─────────────────────────────────────────
typedef struct ObjUpvalue {
    Obj   obj;
    Value* location;           // while open: points into some frame's stack slot
    Value  closed;             // while closed: holds the copied-out value
    struct ObjUpvalue* next;    // intrusive list of currently-open upvalues
} ObjUpvalue;

// ── Closure — a function paired with the upvalues it captured at the
// point it was created. This is the value that actually gets called;
// ObjFunction alone only knows its own bytecode, never its environment. ──
typedef struct ObjClosure {
    Obj          obj;
    ObjFunction* function;
    ObjUpvalue** upvalues;
    int          upvalueCount;
} ObjClosure;

// ── Socket — wraps a native TCP socket handle (listening, connected, or
// accepted). Portable across POSIX (int fd) and Windows (SOCKET handle,
// which is really a pointer-sized unsigned int under the hood). ──────────
#ifdef _WIN32
  #include <basetsd.h>
  typedef UINT_PTR TripSocketHandle;
  #define TRIP_INVALID_SOCKET ((TripSocketHandle)(~0))
#else
  typedef int TripSocketHandle;
  #define TRIP_INVALID_SOCKET ((TripSocketHandle)(-1))
#endif

typedef struct ObjSocket {
    Obj              obj;
    TripSocketHandle handle;
    bool             isListening;  // true if created via tcpListen()
    bool             closed;       // true once tcpClose() has run
    // Max-connection guard (listening sockets only).
    // -1 = unlimited (default). Set by the optional 3rd arg to tcpListen().
    int              maxConns;
    int              activeConns;  // how many accepted clients are still open
    // Back-pointer to the listening socket that accepted this connection,
    // so tcpClose() can decrement activeConns on the parent.
    struct ObjSocket* serverSocket;

    // ── TLS ──────────────────────────────────────────────────────────────
    // ssl == NULL means "plain TCP socket" — every existing tcp* builtin
    // and freeObject() path is untouched for those.
    SSL*     ssl;
    // ctx is loaded once per tlsListen() (server cert+key) or once per
    // tlsConnect() (client verification settings). Accepted client sockets
    // (from tlsAccept()) borrow their parent listening socket's ctx and
    // must NOT free it — ctxOwned tracks who is actually responsible.
    SSL_CTX* ctx;
    bool     ctxOwned;
    // True once the handshake has completed successfully. Lets tlsRead/
    // tlsWrite/tlsClose assert they're not being called on a socket whose
    // handshake never finished (shouldn't happen given how the builtins
    // are structured, but cheap to guard).
    bool     tlsHandshakeDone;
} ObjSocket;

// ── MySQL / MariaDB connection ─────────────────────────────────────────
// Opaque wrapper around a libmysqlclient MYSQL* handle. Mirrors ObjSocket's
// "raw native handle, no GC children to trace" shape.
typedef struct ObjDBConn {
    Obj    obj;
    void*  conn;    // MYSQL* — void* here so object.h doesn't need mysql.h
    bool   closed;
    // Pool handle: conn==NULL, pool points to a MysqlPool* (db_mysql.c)
    bool   isPool;
    void*  pool;
    // Prepared-statement handle: conn==NULL, stmt points to a MYSQL_STMT*
    bool   isStmt;
    void*  stmt;
    // true when this handle's `conn` is a connection checked out from a
    // pool via mysqlPoolGet() — the pool (not this handle) owns the raw
    // MYSQL*, so neither mysqlClose() nor the GC safety net may call
    // mysql_close() on it. Only mysqlPoolRelease() may relinquish it.
    bool   fromPool;
} ObjDBConn;

// ── Class ────────────────────────────────────────────────────────────────
typedef struct ObjClass {
    Obj obj;
    ObjString* name;
    ObjDict* methods;
} ObjClass;

// ── Instance ─────────────────────────────────────────────────────────────
typedef struct ObjInstance {
    Obj obj;
    ObjClass* klass;
    ObjDict* fields;
} ObjInstance;

// ── Bound Method ─────────────────────────────────────────────────────────
typedef struct ObjBoundMethod {
    Obj obj;
    Value receiver; // the instance
    ObjClosure* method;
} ObjBoundMethod;

// ── Type-check / cast macros ──────────────────────────────────────────────
#define OBJ_TYPE(v)         (AS_OBJ(v)->type)
#define IS_STRING(v)        (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRING)
#define IS_LIST(v)          (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_LIST)
#define IS_DICT(v)          (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_DICT)
#define IS_FUNCTION(v)      (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_FUNCTION)
#define IS_CLOSURE(v)       (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CLOSURE)
#define IS_SOCKET(v)        (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_SOCKET)
#define IS_CLASS(v)         (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CLASS)
#define IS_INSTANCE(v)      (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_INSTANCE)
#define IS_BOUND_METHOD(v)  (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_BOUND_METHOD)
#define IS_DB_CONN(v)       (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_DB_CONN)
#define AS_DB_CONN(v)       ((ObjDBConn*)AS_OBJ(v))

#define AS_STRING(v)        ((ObjString*)AS_OBJ(v))
#define AS_LIST(v)          ((ObjList*)AS_OBJ(v))
#define AS_DICT(v)          ((ObjDict*)AS_OBJ(v))
#define AS_FUNCTION(v)      ((ObjFunction*)AS_OBJ(v))
#define AS_CLOSURE(v)       ((ObjClosure*)AS_OBJ(v))
#define AS_SOCKET(v)        ((ObjSocket*)AS_OBJ(v))
#define AS_CLASS(v)         ((ObjClass*)AS_OBJ(v))
#define AS_INSTANCE(v)      ((ObjInstance*)AS_OBJ(v))
#define AS_BOUND_METHOD(v)  ((ObjBoundMethod*)AS_OBJ(v))

// ── Constructors ──────────────────────────────────────────────────────────
ObjString* copyString(const char* chars, int length);
ObjList*   newList();
void       listAppend(ObjList* list, Value value);
void       listFree(ObjList* list);

ObjDict*   newDict();
void       dictSet(ObjDict* dict, ObjString* key, Value value);
bool       dictGet(ObjDict* dict, ObjString* key, Value* out);
bool       dictDelete(ObjDict* dict, ObjString* key);
void       dictFree(ObjDict* dict);
ObjFunction* newFunction(ObjString* name, int arity);
ObjClosure*  newClosure(ObjFunction* function);
ObjUpvalue*  newUpvalue(Value* slot);
ObjSocket*   newSocket(TripSocketHandle handle, bool isListening);
ObjDBConn*   newDBConn(void* conn);
ObjClass*    newClass(ObjString* name);
ObjInstance* newInstance(ObjClass* klass);
ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method);

// Implemented in vm.c, where the platform networking headers live.
void         tripCloseSocketHandle(TripSocketHandle handle);
void         freeObject(Obj* object);

#endif
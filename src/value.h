#ifndef value_h
#define value_h

#include <stdbool.h>

typedef enum {
    VAL_BOOL,
    VAL_NUMBER,   // double — covers int, float, double
    VAL_CHAR,     // single character 'a'
    VAL_NIL,      // nil / null
    VAL_OBJ,      // heap-allocated: string, list, dict
} ValueType;

typedef struct Obj Obj;
typedef struct ObjString ObjString;

typedef struct {
    ValueType type;
    union {
        bool   boolean;
        double number;
        char   character;
        Obj*   obj;
    } as;
} Value;

// ── bool ────────────────────────────────────────────────────────────────
#define IS_BOOL(v)      ((v).type == VAL_BOOL)
#define AS_BOOL(v)      ((v).as.boolean)
#define BOOL_VAL(b)     ((Value){VAL_BOOL,   { .boolean   = (b)  }})

// ── number (int / float / double all share this) ─────────────────────────
#define IS_NUMBER(v)    ((v).type == VAL_NUMBER)
#define AS_NUMBER(v)    ((v).as.number)
#define NUMBER_VAL(n)   ((Value){VAL_NUMBER,  { .number    = (n)  }})

// ── char ────────────────────────────────────────────────────────────────
#define IS_CHAR(v)      ((v).type == VAL_CHAR)
#define AS_CHAR(v)      ((v).as.character)
#define CHAR_VAL(c)     ((Value){VAL_CHAR,    { .character = (c)  }})

// ── nil ─────────────────────────────────────────────────────────────────
#define IS_NIL(v)       ((v).type == VAL_NIL)
#define NIL_VAL         ((Value){VAL_NIL,     { .number    = 0    }})

// ── obj (string / list / dict) ───────────────────────────────────────────
#define IS_OBJ(v)       ((v).type == VAL_OBJ)
#define AS_OBJ(v)       ((v).as.obj)
#define OBJ_VAL(object) ((Value){VAL_OBJ, { .obj = (Obj*)(object) }})

// ── ValueArray (constant pool) ──────────────────────────────────────────
typedef struct {
    int    capacity;
    int    count;
    Value* values;
} ValueArray;

void initValueArray(ValueArray* array);
void writeValueArray(ValueArray* array, Value value);
void freeValueArray(ValueArray* array);

#endif
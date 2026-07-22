#include <stdlib.h>
#include "value.h"
#include "memory.h"

void initValueArray(ValueArray* array) {
    array->values = NULL;
    array->capacity = 0;
    array->count = 0;
}

void writeValueArray(ValueArray* array, Value value) {
    if (array->capacity < array->count + 1) {
        int oldCapacity = array->capacity;
        int newCapacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        array->values = (Value*)reallocate(array->values,
                                           sizeof(Value) * (size_t)oldCapacity,
                                           sizeof(Value) * (size_t)newCapacity);
        array->capacity = newCapacity;
    }
    array->values[array->count] = value;
    array->count++;
}

void freeValueArray(ValueArray* array) {
    reallocate(array->values, sizeof(Value) * (size_t)array->capacity, 0);
    initValueArray(array);
}
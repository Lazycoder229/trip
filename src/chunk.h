#ifndef chunk_h
#define chunk_h

#include <stdint.h>
#include "value.h"

typedef enum {
    OP_CONSTANT,
    // Arithmetic
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_NEGATE,
    OP_MODULO,        // %
    // Comparison
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_NOT,
    // Control flow
    OP_POP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_MATCH_EQUAL,
    // Variables
    OP_DEFINE_GLOBAL,
    OP_DEFINE_LET,
    OP_DEFINE_CONST,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    // Collections
    OP_BUILD_LIST,    // operand = item count; pops N values, pushes ObjList
    OP_BUILD_DICT,    // operand = pair count; pops N*2 key+value pairs, pushes ObjDict
    OP_INDEX_GET,     // list[i] or dict[key]
    OP_INDEX_SET,     // list[i] = v or dict[key] = v
    // Classes
    OP_CLASS,
    OP_METHOD,
    OP_PROPERTY_GET,
    OP_PROPERTY_SET,
    OP_INVOKE,
    // Built-in method calls
    // Stack convention: receiver, [args...], then opcode + 1-byte method-id + 1-byte argc
    OP_CALL_METHOD,
    OP_CALL,
    OP_RETURN_VAL,
    OP_RETURN_NIL,
    // Closures & upvalues
    // OP_CLOSURE operand: 2-byte constant index (the ObjFunction), followed
    // by upvalueCount pairs of (1-byte isLocal, 1-byte index).
    OP_CLOSURE,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_CLOSE_UPVALUE,
    // try/catch/throw/finally
    // OP_TRY_BEGIN operand: 2-byte offset to the start of the catch block,
    //                       followed by 2-byte offset to the finally block
    //                       (0xFFFF = no finally block).
    OP_TRY_BEGIN,
    OP_TRY_END,
    OP_THROW,
    // OP_FINALLY_BEGIN: marks the start of a finally block.
    // No operands — the VM uses the finallyIp stored in TryHandler.
    // Saves a "re-raise pending" flag on the stack so OP_FINALLY_END knows
    // whether to re-throw after the finally body finishes.
    // Stack effect: pushes 1 value (the pending error or NIL_VAL sentinel).
    OP_FINALLY_BEGIN,
    // OP_FINALLY_END: pops the pending-error value.
    // If it is non-nil, re-raises it (re-throw path).
    // Otherwise falls through normally.
    OP_FINALLY_END,
    // Built-in free functions (len, type, int, float, str, char)
    OP_CALL_BUILTIN,  // 1-byte builtin-id
    // nil literal
    OP_NIL,
    // I/O
    OP_PRINT,
    OP_PRINT_NL,
    OP_RETURN,
} OpCode;

// IDs for OP_CALL_BUILTIN
typedef enum {
    BUILTIN_LEN = 0,
    BUILTIN_TYPE,
    BUILTIN_INT,
    BUILTIN_FLOAT,
    BUILTIN_STR,
    BUILTIN_CHAR,
    BUILTIN_ORD,
    BUILTIN_INPUT,
     BUILTIN_RANGE, 
    // File I/O
    BUILTIN_FILE_READ,
    BUILTIN_FILE_WRITE,
    BUILTIN_FILE_APPEND,
    BUILTIN_FILE_EXISTS,
    BUILTIN_FILE_DELETE,
    // CLI args / environment
    BUILTIN_ARGS,
    BUILTIN_GETENV,
    BUILTIN_SCRIPT_PATH,
    // Math / standard library
    BUILTIN_SQRT,
    BUILTIN_POW,
    BUILTIN_ABS,
    BUILTIN_MIN,
    BUILTIN_MAX,
    BUILTIN_FLOOR,
    BUILTIN_CEIL,
    BUILTIN_ROUND,
    BUILTIN_RANDOM,
    BUILTIN_RANDOM_INT,
    // JSON
    BUILTIN_JSON_PARSE,
    BUILTIN_JSON_STRINGIFY,
    // HTTP
    BUILTIN_HTTP_GET,
    BUILTIN_HTTP_POST,
    BUILTIN_HTTP_PUT,
    BUILTIN_HTTP_DELETE,
    BUILTIN_HTTP_PATCH,
    // TCP sockets
    BUILTIN_TCP_LISTEN,
    BUILTIN_TCP_ACCEPT,
    BUILTIN_TCP_CONNECT,
    BUILTIN_TCP_READ,
    BUILTIN_TCP_WRITE,
    BUILTIN_TCP_CLOSE,
    // TLS sockets (OpenSSL-backed, mirror the tcp* builtins above)
    BUILTIN_TLS_LISTEN,   // tlsListen(port, certPath, keyPath [, host [, maxConns]])
    BUILTIN_TLS_ACCEPT,   // tlsAccept(serverSocket) -> handshake-complete client socket
    BUILTIN_TLS_CONNECT,  // tlsConnect(host, port [, verify])
    BUILTIN_TLS_READ,     // tlsRead(socket [, maxBytes [, timeoutMs]])
    BUILTIN_TLS_WRITE,    // tlsWrite(socket, data)
    BUILTIN_TLS_CLOSE,    // tlsClose(socket)
    BUILTIN_SPAWN,
    BUILTIN_YIELD,
    BUILTIN_WAIT_ALL,  // waitAll() -> nil  (blocks main until all spawned fibers finish)
    // HTTP server-side parsing
    BUILTIN_HTTP_PARSE,     // httpParse(rawStr) -> dict {method,path,headers,body,query}
    BUILTIN_HTTP_RESPONSE,  // httpResponse(status, headersDict, body) -> string
    // WebSocket (built on top of TCP sockets + HTTP upgrade)
    BUILTIN_WS_HANDSHAKE,   // wsHandshake(socket, httpReqDict) -> bool   (server-side upgrade)
    BUILTIN_WS_READ,        // wsRead(socket)          -> string | nil     (nil = connection closed)
    BUILTIN_WS_WRITE,       // wsWrite(socket, msg)    -> nil
    BUILTIN_WS_CLOSE,       // wsClose(socket [, code]) -> nil
    // Chunked transfer encoding + Server-Sent Events (RFC 7230 §4.1 / W3C SSE)
    // Chunked lets you stream a response body piece-by-piece without knowing the
    // total Content-Length upfront.  SSE is chunked HTTP with a fixed
    // Content-Type and a line-oriented event format.
    BUILTIN_HTTP_CHUNKED_START, // httpChunkedStart(socket, status, headersDict) -> nil
    BUILTIN_HTTP_CHUNK_WRITE,   // httpChunkWrite(socket, data)                  -> nil
    BUILTIN_HTTP_CHUNK_END,     // httpChunkEnd(socket)                          -> nil
    BUILTIN_SSE_WRITE,          // sseWrite(socket, data [, event [, id]])       -> nil
    // Date/Time
    BUILTIN_TIME,       // time()      -> Unix timestamp (seconds, float)
    BUILTIN_TIME_MS,    // timeMs()    -> Unix timestamp (milliseconds, float)
    BUILTIN_SLEEP,      // sleep(ms)   -> nil  (pauses execution for ms milliseconds)
    // Date/Calendar
    BUILTIN_DATE_NOW,    // dateNow()                    -> dict (local time fields)
    BUILTIN_DATE_UTC,    // dateUTC([timestamp])          -> dict (UTC fields)
    BUILTIN_DATE_LOCAL,  // dateLocal([timestamp])        -> dict (local time fields)
    BUILTIN_DATE_FORMAT, // dateFormat(timestamp, fmt)    -> string (strftime-style)
    BUILTIN_DATE_PARSE,  // dateParse(str, fmt)           -> timestamp float | nil
    BUILTIN_DATE_MAKE,   // dateMake(y,mo,d,h,mi,s)      -> timestamp float
    // Regex (PCRE2)
    BUILTIN_REGEX_MATCH,       // regexMatch(str, pattern)              -> list | nil
    BUILTIN_REGEX_MATCH_ALL,   // regexMatchAll(str, pattern)           -> list of lists
    BUILTIN_REGEX_TEST,        // regexTest(str, pattern)               -> bool
    BUILTIN_REGEX_REPLACE,     // regexReplace(str, pattern, repl)      -> string
    BUILTIN_REGEX_REPLACE_ALL, // regexReplaceAll(str, pattern, repl)   -> string
    BUILTIN_REGEX_SPLIT,       // regexSplit(str, pattern)              -> list
} BuiltinId;

// IDs for OP_CALL_METHOD (method on a receiver type)
typedef enum {
    // String methods
    METHOD_STR_LEN = 0,
    METHOD_STR_UPPER,
    METHOD_STR_LOWER,
    METHOD_STR_TRIM,
    METHOD_STR_SPLIT,
    METHOD_STR_CONTAINS,
    METHOD_STR_STARTS_WITH,
    METHOD_STR_ENDS_WITH,
    METHOD_STR_REPLACE,
    METHOD_STR_SLICE,
    METHOD_STR_FIND,
    METHOD_STR_REPEAT,
    METHOD_STR_CHARS,
    METHOD_STR_FORMAT,
    METHOD_STR_PAD_LEFT,
    METHOD_STR_PAD_RIGHT,
    METHOD_STR_REVERSE,
    METHOD_STR_IS_DIGIT,
    METHOD_STR_IS_ALPHA,
    METHOD_STR_IS_UPPER,
    METHOD_STR_IS_LOWER,
    // List methods
    METHOD_LIST_APPEND,
    METHOD_LIST_POP,
    METHOD_LIST_LEN,
    METHOD_LIST_PUSH,
    METHOD_LIST_INSERT,
    METHOD_LIST_REMOVE,
    METHOD_LIST_CONTAINS,
    METHOD_LIST_REVERSE,
    METHOD_LIST_SORT,
    METHOD_LIST_SLICE,
    METHOD_LIST_JOIN,
    METHOD_LIST_MAP,
    METHOD_LIST_FILTER,
    METHOD_LIST_CLEAR,
    METHOD_LIST_REDUCE,
    METHOD_LIST_FOR_EACH,
    METHOD_LIST_INDEX_OF,
    METHOD_LIST_FLATTEN,
    METHOD_LIST_ZIP,
    METHOD_LIST_ENUMERATE,
    // Dict methods
    METHOD_DICT_GET,
    METHOD_DICT_SET,
    METHOD_DICT_DEL,
    METHOD_DICT_KEYS,
    METHOD_DICT_VALUES,
    METHOD_DICT_HAS,
    METHOD_DICT_LEN,
    METHOD_DICT_CLEAR,
} MethodId;

typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    int*     lines;      // parallel to code[]; lines[i] = source line of code[i]
    ValueArray constants;
} Chunk;

void initChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
int  addConstant(Chunk* chunk, Value value);
void freeChunk(Chunk* chunk);

// Return the source line number recorded for bytecode offset `offset`.
// Returns -1 if the offset is out of range.
int  getLine(const Chunk* chunk, int offset);

#endif
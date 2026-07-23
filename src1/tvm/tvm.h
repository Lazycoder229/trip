#ifndef tvm_h
#define tvm_h

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

#ifndef _WIN32
#  include <unistd.h>
#endif

#ifdef _POSIX_VERSION
extern char* realpath(const char* path, char* resolved_path);
#endif

// Include existing headers
#include "../vm.h"
#include "../compiler/compiler.h"
#include "../chunk.h"
#include "../memory.h"
#include "../object.h"
#include "../cJSON.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <curl/curl.h>
#include <openssl/err.h>

// ── TCP socket platform layer ───────────────────────────────────────────
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define TRIP_SOCK_ERR(s)   ((s) == INVALID_SOCKET)
#  define trip_close(s)      closesocket((SOCKET)(s))
#  define trip_sock_errno()  WSAGetLastError()
#  define trip_would_block(e) ((e) == WSAEWOULDBLOCK)
#  define poll(fds, n, timeout) WSAPoll((fds), (n), (timeout))
   typedef unsigned long nfds_t;
#  define strncasecmp _strnicmp
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <strings.h>
#  define TRIP_SOCK_ERR(s)   ((s) < 0)
#  define trip_close(s)      close((int)(s))
#  define trip_sock_errno()  errno
#  define trip_would_block(e) ((e) == EWOULDBLOCK || (e) == EAGAIN)
#endif

// ── VM Helpers ─────────────────────────────────────────────────────────────
InterpretResult callBuiltin(uint8_t id, uint8_t argc);
InterpretResult callBuiltinIO(uint8_t id, uint8_t argc);
InterpretResult callBuiltinJSON(uint8_t id, uint8_t argc);
InterpretResult callBuiltinHTTP(uint8_t id, uint8_t argc);
InterpretResult callBuiltinTime(uint8_t id, uint8_t argc);
InterpretResult callBuiltinRegex(uint8_t id, uint8_t argc);
InterpretResult callMethod(uint8_t id, uint8_t argc);
InterpretResult callMethodStr(ObjString* self, uint8_t id, uint8_t argc);
InterpretResult callMethodList(ObjList* self, uint8_t id, uint8_t argc);
InterpretResult callMethodDict(ObjDict* self, uint8_t id, uint8_t argc);

// ── Globals ─────────────────────────────────────────────────────────────
extern VM vm;
extern int g_scriptArgc;
extern const char* const* g_scriptArgv;
extern const char* g_scriptPath;
extern bool g_socketLayerReady;

// ── Stack & State Helpers (vm_helpers.c) ──────────────────────────────
void push(Value value);
Value pop(void);
Value peek(int distance);
void resetStack(void);
void growStack(void);
void growFrames(void);

InterpretResult raiseError(const char* format, ...);
InterpretResult raiseValue(Value errVal);
void printStackTrace(void);

bool isFalsy(Value value);
bool valuesEqual(Value a, Value b);
const char* typeName(Value value);
void printValue(Value value);
ObjString* concatStrings(ObjString* a, ObjString* b);

void registerVar(const char* name, bool isConst);
bool isConst(const char* name);
ObjUpvalue* captureUpvalue(Value* local);
void closeUpvalues(Value* last);
char* resolveFilePath(const char* givenPath);

// ── VM Execution (vm_exec.c) ──────────────────────────────────────────
InterpretResult run(void);
InterpretResult finishCurrentFiber(void);

// ── Scheduler (scheduler.c) ───────────────────────────────────────────
void enqueueReady(Fiber* f);
Fiber* dequeueReady(void);
void enqueueBlocked(Fiber* f);
void dequeueBlocked(Fiber* f);
Fiber* nextFiberToRun(void);
InterpretResult blockCurrentFiberOnIO(TripSocketHandle fd, bool forWrite, long timeoutMs);
InterpretResult runUntilAllDone(void);
long long nowMs(void);
void pollBlockedFibers(void);

// ── Networking (net_tcp.c) ────────────────────────────────────────────
void setNonBlocking(TripSocketHandle sock);
void ensureSocketLayer(void);
void tripCloseSocketHandle(TripSocketHandle handle);
InterpretResult callBuiltinTcp(uint8_t id, uint8_t argc);
InterpretResult callBuiltinTls(uint8_t id, uint8_t argc);
InterpretResult callBuiltinServer(uint8_t id, uint8_t argc);
InterpretResult callBuiltinWs(uint8_t id, uint8_t argc);
bool ws_send_all(TripSocketHandle sock, const char* buf, size_t len);

// Sa HTTP declarations section:
void httpMultiInit(void);
void httpMultiPoll(void);
void httpMultiFree(void);
bool httpHasPending(void);
// ── JSON Converters (builtin_json.c) ──────────────────────────────────
Value cjsonToTrip(cJSON* node);
cJSON* tripToCJSON(Value val);

// ── Dispatchers ───────────────────────────────────────────────────────
InterpretResult callMethod(uint8_t id, uint8_t argc);
InterpretResult callBuiltin(uint8_t id, uint8_t argc);

#endif
#include "tvm.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

void setNonBlocking(TripSocketHandle sock) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags != -1) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool g_socketLayerReady = false;
void ensureSocketLayer(void) {
    if (g_socketLayerReady) return;
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    g_socketLayerReady = true;
}

void tripCloseSocketHandle(TripSocketHandle handle) {
    if (handle == TRIP_INVALID_SOCKET) return;
    trip_close(handle);
}

#define TCP_READ_HARD_CAP (1024 * 1024)

InterpretResult callBuiltinTcp(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_TCP_LISTEN: {
            if (argc < 1 || argc > 3)
                return raiseError("tcpListen() expects 1-3 arguments (port [, host [, maxConns]])");
            int maxConns = -1; 
            if (argc == 3) {
                Value mcVal = pop();
                if (!IS_NUMBER(mcVal)) return raiseError("tcpListen() maxConns must be a number");
                maxConns = (int)AS_NUMBER(mcVal);
                if (maxConns < 1) return raiseError("tcpListen() maxConns must be >= 1");
            }
            ObjString* host = NULL;
            if (argc >= 2) {
                Value hostVal = pop();
                if (!IS_STRING(hostVal)) return raiseError("tcpListen() host must be a string");
                host = AS_STRING(hostVal);
            }
            Value portVal = pop();
            if (!IS_NUMBER(portVal)) return raiseError("tcpListen() port must be a number");
            int port = (int)AS_NUMBER(portVal);
            if (port < 1 || port > 65535) return raiseError("tcpListen() port out of range (1-65535)");

            ensureSocketLayer();

            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags    = AI_PASSIVE; 

            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%d", port);

            int gaiErr = getaddrinfo(host ? host->chars : NULL, portStr, &hints, &res);
            if (gaiErr != 0)
                return raiseError("tcpListen() address resolution failed: %s", gai_strerror(gaiErr));

            TripSocketHandle sock = TRIP_INVALID_SOCKET;
            for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
                TripSocketHandle candidate = (TripSocketHandle)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (TRIP_SOCK_ERR(candidate)) continue;

                int yes = 1;
                setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

#ifdef IPV6_V6ONLY
                if (p->ai_family == AF_INET6) {
                    int no = 0;
                    setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));
                }
#endif

                if (bind(candidate, p->ai_addr, (int)p->ai_addrlen) == 0) {
                    sock = candidate;
                    break;
                }
                tripCloseSocketHandle(candidate);
            }
            freeaddrinfo(res);

            if (sock == TRIP_INVALID_SOCKET)
                return raiseError("tcpListen() failed to bind port %d", port);

            if (listen(sock, 128) != 0) {
                tripCloseSocketHandle(sock);
                return raiseError("tcpListen() listen() failed");
            }
            setNonBlocking(sock);

            ObjSocket* obj = newSocket(sock, /*isListening=*/true);
            obj->maxConns = maxConns;
            push((Value){VAL_OBJ, {.obj = (Obj*)obj}});
            break;
        }

        case BUILTIN_TCP_ACCEPT: {
            if (argc != 1) return raiseError("tcpAccept() expects 1 argument (server socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tcpAccept() expects a socket");
            ObjSocket* server = AS_SOCKET(sockVal);
            if (server->closed) return raiseError("tcpAccept() socket is closed");
            if (!server->isListening) return raiseError("tcpAccept() socket is not listening");

            if (server->maxConns != -1 && server->activeConns >= server->maxConns)
                return raiseError("tcpAccept() server is at max connections (%d)", server->maxConns);

            struct sockaddr_storage clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            TripSocketHandle client = (TripSocketHandle)accept(
                server->handle, (struct sockaddr*)&clientAddr, &addrLen);
            if (TRIP_SOCK_ERR(client)) {
                int err = trip_sock_errno();
                if (trip_would_block(err)) {
                    push(sockVal);
                    vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                    return blockCurrentFiberOnIO(server->handle, /*forWrite=*/false, /*timeoutMs=*/-1);
                }
                return raiseError("tcpAccept() failed (errno %d)", err);
            }
            setNonBlocking(client);

            server->activeConns++;

            ObjSocket* obj = newSocket(client, /*isListening=*/false);
            obj->serverSocket = server; 
            push((Value){VAL_OBJ, {.obj = (Obj*)obj}});
            break;
        }

        case BUILTIN_TCP_CONNECT: {
            if (argc != 2) return raiseError("tcpConnect() expects 2 arguments (host, port)");
            Value portVal = pop();
            Value hostVal = pop();
            if (!IS_STRING(hostVal)) return raiseError("tcpConnect() host must be a string");
            if (!IS_NUMBER(portVal)) return raiseError("tcpConnect() port must be a number");
            const char* hostStr = AS_STRING(hostVal)->chars;
            int port = (int)AS_NUMBER(portVal);
            if (port < 1 || port > 65535) return raiseError("tcpConnect() port out of range (1-65535)");

            ensureSocketLayer();

            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%d", port);

            int gaiErr = getaddrinfo(hostStr, portStr, &hints, &res);
            if (gaiErr != 0)
                return raiseError("tcpConnect() address resolution failed: %s", gai_strerror(gaiErr));

            TripSocketHandle sock = TRIP_INVALID_SOCKET;
            for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
                TripSocketHandle candidate = (TripSocketHandle)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (TRIP_SOCK_ERR(candidate)) continue;
                if (connect(candidate, p->ai_addr, (int)p->ai_addrlen) == 0) {
                    sock = candidate;
                    break;
                }
                tripCloseSocketHandle(candidate);
            }
            freeaddrinfo(res);

            if (sock == TRIP_INVALID_SOCKET)
                return raiseError("tcpConnect() failed to connect to %s:%d", hostStr, port);
            setNonBlocking(sock);

            ObjSocket* obj = newSocket(sock, /*isListening=*/false);
            push((Value){VAL_OBJ, {.obj = (Obj*)obj}});
            break;
        }

        case BUILTIN_TCP_READ: {
            if (argc < 1 || argc > 3)
                return raiseError("tcpRead() expects 1-3 arguments (socket [, maxBytes [, timeoutMs]])");
            long timeoutMs = -1;
            if (argc == 3) {
                Value tVal = pop();
                if (!IS_NUMBER(tVal)) return raiseError("tcpRead() timeoutMs must be a number");
                timeoutMs = (long)AS_NUMBER(tVal);
            }
            long maxBytes = 65536;
            if (argc >= 2) {
                Value maxVal = pop();
                if (!IS_NUMBER(maxVal)) return raiseError("tcpRead() maxBytes must be a number");
                maxBytes = (long)AS_NUMBER(maxVal);
                if (maxBytes <= 0) return raiseError("tcpRead() maxBytes must be positive");
                if (maxBytes > TCP_READ_HARD_CAP)
                    return raiseError("tcpRead() maxBytes exceeds hard cap (%d bytes)", TCP_READ_HARD_CAP);
            }
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tcpRead() expects a socket");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (s->closed) return raiseError("tcpRead() socket is closed");

            if (vm.current->timedOut) {
                vm.current->timedOut = false;
                return raiseError("tcpRead() timed out after %ldms of inactivity", timeoutMs);
            }

            char* buf = (char*)malloc((size_t)maxBytes);
            if (!buf) return raiseError("tcpRead() out of memory");

            long n = recv(s->handle, buf, (int)maxBytes, 0);
            if (n < 0) {
                int err = trip_sock_errno();
                free(buf);
                if (trip_would_block(err)) {
                    push(sockVal);
                    if (argc >= 2) push(NUMBER_VAL((double)maxBytes));
                    if (argc == 3) push(NUMBER_VAL((double)timeoutMs));
                    vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                    return blockCurrentFiberOnIO(s->handle, false, timeoutMs);
                }
                return raiseError("tcpRead() failed (errno %d)", err);
            }
            if (n == 0) {
                long n2 = recv(s->handle, buf, (int)maxBytes, 0);
                if (n2 > 0) n = n2;
            }
            ObjString* result = copyString(buf, (int)n);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }

        case BUILTIN_TCP_WRITE: {
            if (argc != 2) return raiseError("tcpWrite() expects 2 arguments (socket, data)");
            Value dataVal = pop();
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tcpWrite() expects a socket");
            if (!IS_STRING(dataVal)) return raiseError("tcpWrite() data must be a string");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (s->closed) return raiseError("tcpWrite() socket is closed");
            ObjString* data = AS_STRING(dataVal);

            const char* ptr    = data->chars;
            int         remain = data->length;

            while (remain > 0) {
                long sent = send(s->handle, ptr, remain, 0);
                if (sent < 0) {
                    int err = trip_sock_errno();
                    if (trip_would_block(err)) {
                        ObjString* rest = copyString(ptr, remain);
                        push(sockVal);
                        push((Value){VAL_OBJ, {.obj = (Obj*)rest}});
                        vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                        return blockCurrentFiberOnIO(s->handle, /*forWrite=*/true, /*timeoutMs=*/-1);
                    }
                    return raiseError("tcpWrite() failed (errno %d)", err);
                }
                ptr    += sent;
                remain -= (int)sent;
            }
            push(NUMBER_VAL((double)data->length));
            break;
        }

        case BUILTIN_TCP_CLOSE: {
            if (argc != 1) return raiseError("tcpClose() expects 1 argument (socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tcpClose() expects a socket");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (!s->closed) {
                tripCloseSocketHandle(s->handle);
                s->closed = true;
                if (s->serverSocket != NULL && !s->serverSocket->closed) {
                    s->serverSocket->activeConns--;
                }
            }
            push((Value){VAL_NIL, {.number = 0}});
            break;
        }

        default:
            return raiseError("Unknown TCP builtin %d", id);
    }
    return INTERPRET_OK;
}

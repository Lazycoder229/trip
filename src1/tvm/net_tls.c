#include "tvm.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

#define TCP_READ_HARD_CAP (1024 * 1024)

typedef enum {
    TLS_HS_DONE,
    TLS_HS_WANT_READ,
    TLS_HS_WANT_WRITE,
    TLS_HS_ERROR
} TlsHandshakeOutcome;

static TlsHandshakeOutcome tlsHandshakeAttempt(ObjSocket* s, bool isServer,
                                                char* errbuf, size_t errbufLen) {
    int r = isServer ? SSL_accept(s->ssl) : SSL_connect(s->ssl);
    if (r == 1) {
        s->tlsHandshakeDone = true;
        return TLS_HS_DONE;
    }

    int sslErr = SSL_get_error(s->ssl, r);
    if (sslErr == SSL_ERROR_WANT_READ)  return TLS_HS_WANT_READ;
    if (sslErr == SSL_ERROR_WANT_WRITE) return TLS_HS_WANT_WRITE;

    unsigned long e = ERR_get_error();
    if (e != 0) {
        ERR_error_string_n(e, errbuf, errbufLen);
    } else if (errbufLen > 0) {
        errbuf[0] = '\0';
    }
    return TLS_HS_ERROR;
}

InterpretResult callBuiltinTls(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_TLS_LISTEN: {
            if (argc < 3 || argc > 5)
                return raiseError("tlsListen() expects 3-5 arguments (port, certPath, keyPath [, host [, maxConns]])");

            int maxConns = -1;
            if (argc == 5) {
                Value mcVal = pop();
                if (!IS_NUMBER(mcVal)) return raiseError("tlsListen() maxConns must be a number");
                maxConns = (int)AS_NUMBER(mcVal);
                if (maxConns < 1) return raiseError("tlsListen() maxConns must be >= 1");
            }
            ObjString* host = NULL;
            if (argc >= 4) {
                Value hostVal = pop();
                if (!IS_STRING(hostVal)) return raiseError("tlsListen() host must be a string");
                host = AS_STRING(hostVal);
            }
            Value keyVal = pop();
            Value certVal = pop();
            if (!IS_STRING(certVal)) return raiseError("tlsListen() certPath must be a string");
            if (!IS_STRING(keyVal))  return raiseError("tlsListen() keyPath must be a string");
            Value portVal = pop();
            if (!IS_NUMBER(portVal)) return raiseError("tlsListen() port must be a number");
            int port = (int)AS_NUMBER(portVal);
            if (port < 1 || port > 65535) return raiseError("tlsListen() port out of range (1-65535)");

            ensureSocketLayer();

            SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
            if (!ctx) return raiseError("tlsListen() failed to create SSL context");
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            if (SSL_CTX_use_certificate_chain_file(ctx, AS_STRING(certVal)->chars) <= 0) {
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() failed to load certificate \"%s\"", AS_STRING(certVal)->chars);
            }
            if (SSL_CTX_use_PrivateKey_file(ctx, AS_STRING(keyVal)->chars, SSL_FILETYPE_PEM) <= 0) {
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() failed to load private key \"%s\"", AS_STRING(keyVal)->chars);
            }
            if (!SSL_CTX_check_private_key(ctx)) {
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() certificate/private key mismatch");
            }

            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags    = AI_PASSIVE;

            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%d", port);

            int gaiErr = getaddrinfo(host ? host->chars : NULL, portStr, &hints, &res);
            if (gaiErr != 0) {
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() address resolution failed: %s", gai_strerror(gaiErr));
            }

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

            if (sock == TRIP_INVALID_SOCKET) {
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() failed to bind port %d", port);
            }
            if (listen(sock, 128) != 0) {
                tripCloseSocketHandle(sock);
                SSL_CTX_free(ctx);
                return raiseError("tlsListen() listen() failed");
            }
            setNonBlocking(sock);

            ObjSocket* obj = newSocket(sock, /*isListening=*/true);
            obj->maxConns = maxConns;
            obj->ctx      = ctx;
            obj->ctxOwned = true;
            push((Value){VAL_OBJ, {.obj = (Obj*)obj}});
            break;
        }

        case BUILTIN_TLS_ACCEPT: {
            if (argc != 1) return raiseError("tlsAccept() expects 1 argument (server socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tlsAccept() expects a socket");
            ObjSocket* s = AS_SOCKET(sockVal);

            if (!s->isListening && s->ssl != NULL && !s->tlsHandshakeDone) {
                char errbuf[256];
                TlsHandshakeOutcome outcome = tlsHandshakeAttempt(s, /*isServer=*/true, errbuf, sizeof(errbuf));
                if (outcome == TLS_HS_DONE) { push(sockVal); break; }
                if (outcome == TLS_HS_ERROR)
                    return raiseError("tlsAccept() handshake failed: %s", errbuf[0] ? errbuf : "unknown error");
                push(sockVal);
                vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                return blockCurrentFiberOnIO(s->handle, outcome == TLS_HS_WANT_WRITE, -1);
            }

            ObjSocket* server = s;
            if (server->closed) return raiseError("tlsAccept() socket is closed");
            if (!server->isListening) return raiseError("tlsAccept() socket is not listening");
            if (server->ctx == NULL) return raiseError("tlsAccept() socket was not created by tlsListen()");
            if (server->maxConns != -1 && server->activeConns >= server->maxConns)
                return raiseError("tlsAccept() server is at max connections (%d)", server->maxConns);

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
                return raiseError("tlsAccept() failed (errno %d)", err);
            }
            setNonBlocking(client);
            server->activeConns++;

            ObjSocket* obj = newSocket(client, /*isListening=*/false);
            obj->serverSocket = server;
            obj->ctx          = server->ctx;
            obj->ssl          = SSL_new(obj->ctx);
            if (obj->ssl == NULL) return raiseError("tlsAccept() failed to allocate SSL session");
            SSL_set_fd(obj->ssl, client);
            SSL_set_mode(obj->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);

            Value clientVal = (Value){VAL_OBJ, {.obj = (Obj*)obj}};

            char errbuf[256];
            TlsHandshakeOutcome outcome = tlsHandshakeAttempt(obj, /*isServer=*/true, errbuf, sizeof(errbuf));
            if (outcome == TLS_HS_DONE) { push(clientVal); break; }
            if (outcome == TLS_HS_ERROR)
                return raiseError("tlsAccept() handshake failed: %s", errbuf[0] ? errbuf : "unknown error");

            push(clientVal);
            vm.current->frames[vm.current->frameCount - 1].ip -= 3;
            return blockCurrentFiberOnIO(obj->handle, outcome == TLS_HS_WANT_WRITE, -1);
        }

        case BUILTIN_TLS_CONNECT: {
            if (argc != 2 && argc != 3)
                return raiseError("tlsConnect() expects 2-3 arguments (host, port [, verifyPeer])");

            Value verifyVal = (argc == 3) ? pop() : BOOL_VAL(true);
            Value portVal = pop();
            Value hostVal = pop();

            if (IS_SOCKET(hostVal)) {
                ObjSocket* s = AS_SOCKET(hostVal);
                char errbuf[256];
                TlsHandshakeOutcome outcome = tlsHandshakeAttempt(s, /*isServer=*/false, errbuf, sizeof(errbuf));
                if (outcome == TLS_HS_DONE) { push(hostVal); break; }
                if (outcome == TLS_HS_ERROR)
                    return raiseError("tlsConnect() handshake failed: %s", errbuf[0] ? errbuf : "unknown error");
                push(hostVal);
                push(NIL_VAL);
                if (argc == 3) push(NIL_VAL);
                vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                return blockCurrentFiberOnIO(s->handle, outcome == TLS_HS_WANT_WRITE, -1);
            }

            if (!IS_STRING(hostVal)) return raiseError("tlsConnect() host must be a string");
            if (!IS_NUMBER(portVal)) return raiseError("tlsConnect() port must be a number");
            const char* hostStr = AS_STRING(hostVal)->chars;
            int port = (int)AS_NUMBER(portVal);
            if (port < 1 || port > 65535) return raiseError("tlsConnect() port out of range (1-65535)");
            bool verifyPeer = true;
            if (argc == 3) {
                if (!IS_BOOL(verifyVal)) return raiseError("tlsConnect() verifyPeer must be a bool");
                verifyPeer = AS_BOOL(verifyVal);
            }

            ensureSocketLayer();

            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%d", port);

            int gaiErr = getaddrinfo(hostStr, portStr, &hints, &res);
            if (gaiErr != 0)
                return raiseError("tlsConnect() address resolution failed: %s", gai_strerror(gaiErr));

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
                return raiseError("tlsConnect() failed to connect to %s:%d", hostStr, port);
            setNonBlocking(sock);

            SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) { tripCloseSocketHandle(sock); return raiseError("tlsConnect() failed to create SSL context"); }
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            if (verifyPeer) {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                SSL_CTX_set_default_verify_paths(ctx);
            } else {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
            }

            ObjSocket* obj = newSocket(sock, /*isListening=*/false);
            obj->ctx      = ctx;
            obj->ctxOwned = true;
            obj->ssl      = SSL_new(ctx);
            if (!obj->ssl) return raiseError("tlsConnect() failed to allocate SSL session");
            SSL_set_fd(obj->ssl, sock);
            SSL_set_mode(obj->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
            SSL_set_tlsext_host_name(obj->ssl, hostStr);

            Value objVal = (Value){VAL_OBJ, {.obj = (Obj*)obj}};

            char errbuf[256];
            TlsHandshakeOutcome outcome = tlsHandshakeAttempt(obj, /*isServer=*/false, errbuf, sizeof(errbuf));
            if (outcome == TLS_HS_DONE) { push(objVal); break; }
            if (outcome == TLS_HS_ERROR)
                return raiseError("tlsConnect() handshake failed: %s", errbuf[0] ? errbuf : "unknown error");

            push(objVal);
            push(NIL_VAL);
            if (argc == 3) push(NIL_VAL);
            vm.current->frames[vm.current->frameCount - 1].ip -= 3;
            return blockCurrentFiberOnIO(obj->handle, outcome == TLS_HS_WANT_WRITE, -1);
        }

        case BUILTIN_TLS_READ: {
            if (argc < 1 || argc > 3)
                return raiseError("tlsRead() expects 1-3 arguments (socket [, maxBytes [, timeoutMs]])");
            long timeoutMs = -1;
            if (argc == 3) {
                Value tVal = pop();
                if (!IS_NUMBER(tVal)) return raiseError("tlsRead() timeoutMs must be a number");
                timeoutMs = (long)AS_NUMBER(tVal);
            }
            long maxBytes = 65536;
            if (argc >= 2) {
                Value maxVal = pop();
                if (!IS_NUMBER(maxVal)) return raiseError("tlsRead() maxBytes must be a number");
                maxBytes = (long)AS_NUMBER(maxVal);
                if (maxBytes <= 0) return raiseError("tlsRead() maxBytes must be positive");
                if (maxBytes > TCP_READ_HARD_CAP)
                    return raiseError("tlsRead() maxBytes exceeds hard cap (%d bytes)", TCP_READ_HARD_CAP);
            }
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tlsRead() expects a socket");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (s->closed) return raiseError("tlsRead() socket is closed");
            if (s->ssl == NULL) return raiseError("tlsRead() socket was not created by tlsConnect()/tlsAccept()");

            if (vm.current->timedOut) {
                vm.current->timedOut = false;
                return raiseError("tlsRead() timed out after %ldms of inactivity", timeoutMs);
            }

            char* buf = (char*)malloc((size_t)maxBytes);
            if (!buf) return raiseError("tlsRead() out of memory");

            int n = SSL_read(s->ssl, buf, (int)maxBytes);
            if (n <= 0) {
                int sslErr = SSL_get_error(s->ssl, n);
                free(buf);
                if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
                    push(sockVal);
                    if (argc >= 2) push(NUMBER_VAL((double)maxBytes));
                    if (argc == 3) push(NUMBER_VAL((double)timeoutMs));
                    vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                    return blockCurrentFiberOnIO(s->handle, sslErr == SSL_ERROR_WANT_WRITE, timeoutMs);
                }
                if (sslErr == SSL_ERROR_ZERO_RETURN) {
                    ObjString* result = copyString("", 0);
                    push((Value){VAL_OBJ, {.obj = (Obj*)result}});
                    break;
                }
                unsigned long e = ERR_get_error();
                char errbuf[256] = {0};
                if (e != 0) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                return raiseError("tlsRead() failed: %s", errbuf[0] ? errbuf : "connection reset");
            }
            ObjString* result = copyString(buf, n);
            free(buf);
            push((Value){VAL_OBJ, {.obj = (Obj*)result}});
            break;
        }

        case BUILTIN_TLS_WRITE: {
            if (argc != 2) return raiseError("tlsWrite() expects 2 arguments (socket, data)");
            Value dataVal = pop();
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tlsWrite() expects a socket");
            if (!IS_STRING(dataVal)) return raiseError("tlsWrite() data must be a string");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (s->closed) return raiseError("tlsWrite() socket is closed");
            if (s->ssl == NULL) return raiseError("tlsWrite() socket was not created by tlsConnect()/tlsAccept()");
            ObjString* data = AS_STRING(dataVal);

            const char* ptr    = data->chars;
            int         remain = data->length;

            while (remain > 0) {
                int sent = SSL_write(s->ssl, ptr, remain);
                if (sent <= 0) {
                    int sslErr = SSL_get_error(s->ssl, sent);
                    if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
                        ObjString* rest = copyString(ptr, remain);
                        push(sockVal);
                        push((Value){VAL_OBJ, {.obj = (Obj*)rest}});
                        vm.current->frames[vm.current->frameCount - 1].ip -= 3;
                        return blockCurrentFiberOnIO(s->handle, sslErr == SSL_ERROR_WANT_WRITE, -1);
                    }
                    unsigned long e = ERR_get_error();
                    char errbuf[256] = {0};
                    if (e != 0) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                    return raiseError("tlsWrite() failed: %s", errbuf[0] ? errbuf : "connection reset");
                }
                ptr    += sent;
                remain -= sent;
            }
            push(NUMBER_VAL((double)data->length));
            break;
        }

        case BUILTIN_TLS_CLOSE: {
            if (argc != 1) return raiseError("tlsClose() expects 1 argument (socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("tlsClose() expects a socket");
            ObjSocket* s = AS_SOCKET(sockVal);
            if (!s->closed) {
                if (s->ssl != NULL) {
                    SSL_shutdown(s->ssl);
                    SSL_free(s->ssl);
                    s->ssl = NULL;
                }
                if (s->ctxOwned && s->ctx != NULL) {
                    SSL_CTX_free(s->ctx);
                    s->ctx = NULL;
                }
                tripCloseSocketHandle(s->handle);
                s->closed = true;
                if (s->serverSocket != NULL && !s->serverSocket->closed) {
                    s->serverSocket->activeConns--;
                }
            }
            push(NIL_VAL);
            break;
        }

        default:
            return raiseError("Unknown TLS builtin %d", id);
    }
    return INTERPRET_OK;
}

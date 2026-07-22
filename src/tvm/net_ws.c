#include "tvm.h"
#include <string.h>
#include <stdlib.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_ROL32(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void ws_sha1(const uint8_t* msg, size_t msgLen, uint8_t out[20]) {
    uint32_t h0=0x67452301u, h1=0xEFCDAB89u,
             h2=0x98BADCFEu, h3=0x10325476u, h4=0xC3D2E1F0u;

    size_t bitLen  = msgLen * 8;
    size_t padLen  = ((msgLen + 8) / 64 + 1) * 64;
    uint8_t* padded = (uint8_t*)calloc(padLen, 1);
    if (!padded) return;
    memcpy(padded, msg, msgLen);
    padded[msgLen] = 0x80;
    for (int i = 0; i < 8; i++)
        padded[padLen - 1 - i] = (uint8_t)(bitLen >> (i * 8));

    for (size_t off = 0; off < padLen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)padded[off+i*4+0]<<24)|((uint32_t)padded[off+i*4+1]<<16)
                  |((uint32_t)padded[off+i*4+2]<< 8)|((uint32_t)padded[off+i*4+3]);
        }
        for (int i = 16; i < 80; i++)
            w[i] = WS_ROL32(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);

        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i<20){f=(b&c)|(~b&d);      k=0x5A827999u;}
            else if (i<40){f=b^c^d;              k=0x6ED9EBA1u;}
            else if (i<60){f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu;}
            else          {f=b^c^d;              k=0xCA62C1D6u;}
            uint32_t tmp=WS_ROL32(a,5)+f+e+k+w[i];
            e=d; d=c; c=WS_ROL32(b,30); b=a; a=tmp;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }
    free(padded);

    uint32_t digest[5]={h0,h1,h2,h3,h4};
    for (int i=0;i<5;i++){
        out[i*4+0]=(uint8_t)(digest[i]>>24); out[i*4+1]=(uint8_t)(digest[i]>>16);
        out[i*4+2]=(uint8_t)(digest[i]>> 8); out[i*4+3]=(uint8_t)(digest[i]);
    }
}
#undef WS_ROL32

static char* ws_base64(const uint8_t* in, size_t inLen) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outLen = ((inLen+2)/3)*4 + 1;
    char* out = (char*)malloc(outLen);
    if (!out) return NULL;
    size_t i=0, j=0;
    while (i+3 <= inLen) {
        uint32_t v=((uint32_t)in[i]<<16)|((uint32_t)in[i+1]<<8)|in[i+2];
        out[j++]=b64[(v>>18)&63]; out[j++]=b64[(v>>12)&63];
        out[j++]=b64[(v>> 6)&63]; out[j++]=b64[(v    )&63];
        i+=3;
    }
    if (inLen-i==1) {
        uint32_t v=(uint32_t)in[i]<<16;
        out[j++]=b64[(v>>18)&63]; out[j++]=b64[(v>>12)&63];
        out[j++]='='; out[j++]='=';
    } else if (inLen-i==2) {
        uint32_t v=((uint32_t)in[i]<<16)|((uint32_t)in[i+1]<<8);
        out[j++]=b64[(v>>18)&63]; out[j++]=b64[(v>>12)&63];
        out[j++]=b64[(v>> 6)&63]; out[j++]='=';
    }
    out[j]='\0';
    return out;
}

InterpretResult callBuiltinWs(uint8_t id, uint8_t argc) {
    switch (id) {
        case BUILTIN_WS_HANDSHAKE: {
            if (argc != 2)
                return raiseError("wsHandshake() expects 2 arguments (socket, httpReqDict)");
            Value reqVal  = pop();
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("wsHandshake() first arg must be a socket");
            if (!IS_DICT(reqVal))    return raiseError("wsHandshake() second arg must be a dict (from httpParse)");
            ObjSocket* sock = AS_SOCKET(sockVal);
            if (sock->closed) return raiseError("wsHandshake() socket is closed");
            ObjDict*   req  = AS_DICT(reqVal);

            ObjString* keyName   = copyString("sec-websocket-key", 17);
            Value      headersDV;
            ObjString* wsKeyStr  = NULL;
            if (dictGet(req, copyString("headers", 7), &headersDV) && IS_DICT(headersDV)) {
                Value kv;
                if (dictGet(AS_DICT(headersDV), keyName, &kv) && IS_STRING(kv))
                    wsKeyStr = AS_STRING(kv);
            }
            if (!wsKeyStr)
                return raiseError("wsHandshake() missing Sec-WebSocket-Key header");

            size_t nonceLen = (size_t)wsKeyStr->length;
            size_t guidLen  = strlen(WS_GUID);
            size_t concatLen = nonceLen + guidLen;
            char* concat = (char*)malloc(concatLen + 1);
            if (!concat) return raiseError("wsHandshake() out of memory");
            memcpy(concat,            wsKeyStr->chars, nonceLen);
            memcpy(concat + nonceLen, WS_GUID,         guidLen);
            concat[concatLen] = '\0';

            uint8_t digest[20];
            ws_sha1((const uint8_t*)concat, concatLen, digest);
            free(concat);

            char* acceptKey = ws_base64(digest, 20);
            if (!acceptKey) return raiseError("wsHandshake() out of memory");

            char resp[512];
            int  respLen = snprintf(resp, sizeof(resp),
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n"
                "\r\n",
                acceptKey);
            free(acceptKey);

            if (respLen < 0 || (size_t)respLen >= sizeof(resp))
                return raiseError("wsHandshake() response buffer overflow");

            if (!ws_send_all(sock->handle, resp, (size_t)respLen))
                return raiseError("wsHandshake() failed to send 101 response");

            push(BOOL_VAL(true));
            break;
        }

        case BUILTIN_WS_READ: {
            if (argc != 1) return raiseError("wsRead() expects 1 argument (socket)");
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("wsRead() expects a socket");
            ObjSocket* sock = AS_SOCKET(sockVal);
            if (sock->closed) return raiseError("wsRead() socket is closed");

            #define WS_RECV_EXACT(dst, need) do { \
                size_t _got = 0; \
                while (_got < (need)) { \
                    long _n = recv(sock->handle, (char*)(dst) + _got, (int)((need) - _got), 0); \
                    if (_n <= 0) goto ws_read_eof; \
                    _got += (size_t)_n; \
                } \
            } while(0)

            size_t  msgCap = 4096, msgLen = 0;
            uint8_t* msg = (uint8_t*)malloc(msgCap);
            if (!msg) return raiseError("wsRead() out of memory");

            for (;;) {
                uint8_t hdr[2];
                WS_RECV_EXACT(hdr, 2);

                bool    fin    = (hdr[0] & 0x80) != 0;
                uint8_t opcode = (hdr[0] & 0x0F);
                bool    masked = (hdr[1] & 0x80) != 0;
                uint64_t payLen = (hdr[1] & 0x7F);

                if (payLen == 126) {
                    uint8_t ext[2]; WS_RECV_EXACT(ext, 2);
                    payLen = ((uint64_t)ext[0] << 8) | ext[1];
                } else if (payLen == 127) {
                    uint8_t ext[8]; WS_RECV_EXACT(ext, 8);
                    payLen = 0;
                    for (int i = 0; i < 8; i++) payLen = (payLen << 8) | ext[i];
                }

                if (payLen > 16 * 1024 * 1024) {
                    free(msg);
                    return raiseError("wsRead() frame payload too large (%llu bytes)",
                                      (unsigned long long)payLen);
                }

                uint8_t maskKey[4] = {0, 0, 0, 0};
                if (masked) WS_RECV_EXACT(maskKey, 4);

                uint8_t* payload = (uint8_t*)malloc((size_t)payLen + 1);
                if (!payload) { free(msg); return raiseError("wsRead() out of memory"); }
                WS_RECV_EXACT(payload, (size_t)payLen);

                if (masked) {
                    for (uint64_t i = 0; i < payLen; i++)
                        payload[i] ^= maskKey[i & 3];
                }

                if (opcode == 0x8) {
                    uint8_t closeFrame[4] = {0x88, 0x02, 0x03, 0xE8};
                    ws_send_all(sock->handle, (char*)closeFrame, 4);
                    free(payload); free(msg);
                    push(NIL_VAL);
                    break;
                }
                if (opcode == 0x9) {
                    size_t pongPayLen = (size_t)payLen;
                    uint8_t pongHdr[2] = { 0x8A, (uint8_t)(pongPayLen & 0x7F) };
                    ws_send_all(sock->handle, (char*)pongHdr, 2);
                    if (pongPayLen > 0)
                        ws_send_all(sock->handle, (char*)payload, pongPayLen);
                    free(payload);
                    continue;
                }
                if (opcode == 0xA) {
                    free(payload); continue;
                }

                size_t needed = msgLen + (size_t)payLen;
                if (needed > msgCap) {
                    while (msgCap < needed) msgCap *= 2;
                    uint8_t* tmp = (uint8_t*)realloc(msg, msgCap);
                    if (!tmp) { free(payload); free(msg); return raiseError("wsRead() out of memory"); }
                    msg = tmp;
                }
                memcpy(msg + msgLen, payload, (size_t)payLen);
                msgLen += (size_t)payLen;
                free(payload);

                if (fin) {
                    ObjString* result = copyString((char*)msg, (int)msgLen);
                    free(msg);
                    push((Value){VAL_OBJ, {.obj = (Obj*)result}});
                    break;
                }
            }

            #undef WS_RECV_EXACT
            break;

            ws_read_eof:
            free(msg);
            push(NIL_VAL);
            break;
        }

        case BUILTIN_WS_WRITE: {
            if (argc != 2) return raiseError("wsWrite() expects 2 arguments (socket, message)");
            Value msgVal  = pop();
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("wsWrite() first arg must be a socket");
            if (!IS_STRING(msgVal))  return raiseError("wsWrite() second arg must be a string");
            ObjSocket*  sock   = AS_SOCKET(sockVal);
            ObjString*  msgStr = AS_STRING(msgVal);
            if (sock->closed) return raiseError("wsWrite() socket is closed");

            size_t payLen = (size_t)msgStr->length;

            uint8_t header[10];
            size_t  headerLen;
            header[0] = 0x81;

            if (payLen < 126) {
                header[1]  = (uint8_t)payLen;
                headerLen  = 2;
            } else if (payLen < 65536) {
                header[1]  = 126;
                header[2]  = (uint8_t)(payLen >> 8);
                header[3]  = (uint8_t)(payLen);
                headerLen  = 4;
            } else {
                header[1]  = 127;
                for (int i = 0; i < 8; i++)
                    header[2 + i] = (uint8_t)(payLen >> ((7 - i) * 8));
                headerLen  = 10;
            }

            if (!ws_send_all(sock->handle, (char*)header, headerLen) ||
                !ws_send_all(sock->handle, msgStr->chars, payLen)) {
                return raiseError("wsWrite() send failed");
            }
            push(NIL_VAL);
            break;
        }

        case BUILTIN_WS_CLOSE: {
            if (argc < 1 || argc > 2)
                return raiseError("wsClose() expects 1-2 arguments (socket [, code])");
            uint16_t code = 1000;
            if (argc == 2) {
                Value codeVal = pop();
                if (!IS_NUMBER(codeVal)) return raiseError("wsClose() code must be a number");
                code = (uint16_t)AS_NUMBER(codeVal);
            }
            Value sockVal = pop();
            if (!IS_SOCKET(sockVal)) return raiseError("wsClose() expects a socket");
            ObjSocket* sock = AS_SOCKET(sockVal);

            if (!sock->closed) {
                uint8_t frame[4];
                frame[0] = 0x88;
                frame[1] = 0x02;
                frame[2] = (uint8_t)(code >> 8);
                frame[3] = (uint8_t)(code);
                ws_send_all(sock->handle, (char*)frame, 4);

                if (sock->serverSocket != NULL && !sock->serverSocket->closed)
                    sock->serverSocket->activeConns--;

                tripCloseSocketHandle(sock->handle);
                sock->closed = true;
            }
            push(NIL_VAL);
            break;
        }

        default:
            return raiseError("Unknown WebSocket builtin %d", id);
    }
    return INTERPRET_OK;
}

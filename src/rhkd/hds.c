// SPDX-License-Identifier: GPL-3.0-or-later
//
// HomeKit Data Stream transport, ported from HAP-NodeJS src/lib/datastream/DataStreamManagement.ts and
// DataStreamServer.ts (Apache-2.0, Copyright The HAP-NodeJS contributors).
//
// Setup (HAP run loop): the controller writes SetupDataStreamTransport {01 command=start, 02 transport=HDS,
// 03 controller salt (32)}. We pick an accessory salt and derive two keys from the HAP session's pair-verify
// shared secret: HKDF-SHA512(salt = controller salt || accessory salt, info "HDS-Read-Encryption-Key") for
// accessory->controller and "HDS-Write-Encryption-Key" for controller->accessory. The write response is
// {01 status, 02 {01 TCP port (u16 LE)}, 03 accessory salt}. The controller then connects within 10 s.
//
// Transport (HDS thread): frames are [type 1][length u24 BE] + ChaCha20-Poly1305(payload) + 16-byte tag, with
// the 4-byte header as AAD and a 64-bit little-endian counter nonce per direction. The first frame is matched
// to a prepared session by trying its key. Payload = [header length u8][OPACK header dict][OPACK message dict].
// The first message must be the request control/hello, answered with an empty success response.

#include "hds.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "HAP+Internal.h"
#include "HAPCrypto.h"
#include "HAPPlatformRandomNumber.h"
#include "HAPUUID.h"
#include "tlvb.h"

static const HAPLogObject logObject = { .subsystem = "rhkd", .category = "DataStream" };

#define MAX_PREPARED 4
#define MAX_CONNS    4
#define CONNECT_TIMEOUT_MS 10000
#define HELLO_TIMEOUT_MS   10000
#define RX_MAX (4 + HDS_MAX_PAYLOAD + 16)

typedef struct {
    bool used;
    HAPSessionRef* hapSession;
    uint8_t c2aKey[32], a2cKey[32];
    uint64_t deadline;
} prepared_t;

struct hds_conn {
    bool used, ready, closing;
    int fd;
    HAPSessionRef* hapSession;
    uint8_t c2aKey[32], a2cKey[32];
    uint64_t c2aNonce, a2cNonce;
    bool keyed;
    uint64_t helloDeadline;
    uint8_t* rx;
    size_t rxLen;
    pthread_mutex_t txLock;
};

static struct {
    pthread_mutex_t lock;
    int listenFd;
    uint16_t port;
    int wakePipe[2];
    prepared_t prepared[MAX_PREPARED];
    hds_conn_t conns[MAX_CONNS];
    hds_message_fn onMessage;
    hds_closed_fn onClosed;

    // SetupDataStreamTransport read-back (HAP run loop only).
    uint8_t response[128];
    size_t responseLen, responseLenNoSalt;
    bool responseFullOnce;
} hds = { .lock = PTHREAD_MUTEX_INITIALIZER, .listenFd = -1, .wakePipe = { -1, -1 } };

static uint64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

static void Wake(void) {
    if (hds.wakePipe[1] >= 0) {
        uint8_t b = 1;
        (void) !write(hds.wakePipe[1], &b, 1);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

static void Nonce(uint64_t counter, uint8_t n[8]) {
    for (int i = 0; i < 8; i++)
        n[i] = (uint8_t) (counter >> (8 * i));
}

static bool WriteAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, 5000) <= 0)
                    return false;
                continue;
            }
            return false;
        }
        p += w;
        n -= (size_t) w;
    }
    return true;
}

// Encodes header + message into one encrypted frame and sends it.
static int SendFrame(hds_conn_t* c, const uint8_t* hdr, size_t hdrLen, const uint8_t* msg, size_t msgLen) {
    size_t payloadLen = 1 + hdrLen + msgLen;
    if (hdrLen > 255 || payloadLen > HDS_MAX_PAYLOAD)
        return -1;
    uint8_t* frame = malloc(4 + payloadLen + 16);
    if (!frame)
        return -1;
    frame[0] = 1;
    frame[1] = (uint8_t) (payloadLen >> 16);
    frame[2] = (uint8_t) (payloadLen >> 8);
    frame[3] = (uint8_t) payloadLen;
    uint8_t* plain = frame + 4;
    plain[0] = (uint8_t) hdrLen;
    memcpy(plain + 1, hdr, hdrLen);
    if (msgLen)
        memcpy(plain + 1 + hdrLen, msg, msgLen);

    int ret = -1;
    pthread_mutex_lock(&c->txLock);
    if (c->used && !c->closing && c->fd >= 0) {
        uint8_t n[8];
        Nonce(c->a2cNonce++, n);
        HAP_chacha20_poly1305_encrypt_aad(plain + payloadLen, plain, plain, payloadLen, frame, 4, n, sizeof n,
                                          c->a2cKey);
        ret = WriteAll(c->fd, frame, 4 + payloadLen + 16) ? 0 : -1;
    }
    pthread_mutex_unlock(&c->txLock);
    free(frame);
    if (ret)
        HAPLogError(&logObject, "send failed on HDS connection %d", c->fd);
    return ret;
}

// Bytes on the connection's socket that the controller has not acknowledged yet (Linux SIOCOUTQ = TIOCOUTQ:
// unsent + unacked). -1 if the connection is gone.
static long Unacked(hds_conn_t* c) {
    int n = -1;
    pthread_mutex_lock(&c->txLock);
    if (c->used && !c->closing && c->fd >= 0 && ioctl(c->fd, TIOCOUTQ, &n) != 0)
        n = -1;
    pthread_mutex_unlock(&c->txLock);
    return n;
}

int hds_wait_unacked(hds_conn_t* c, size_t maxBytes, unsigned timeoutMs, unsigned* waitedMs) {
    uint64_t start = NowMs();
    for (;;) {
        long n = Unacked(c);
        if (n < 0)
            return -1;
        uint64_t waited = NowMs() - start;
        if ((size_t) n <= maxBytes) {
            if (waitedMs)
                *waitedMs += (unsigned) waited;
            return 0;
        }
        if (waited >= timeoutMs) {
            HAPLogError(&logObject, "HDS connection %d: %ld bytes unacknowledged after %u ms", c->fd, n, timeoutMs);
            return -1;
        }
        usleep(10 * 1000);
    }
}

static const uint8_t kEmptyDict[] = { 0xE0 };

int hds_send_response(hds_conn_t* c, const char* protocol, const char* topic, int64_t id, int status,
                      const uint8_t* msg, size_t msgLen) {
    uint8_t hb[160];
    op_writer_t w;
    op_w_init(&w, hb, sizeof hb);
    op_w_dict(&w, 4);
    op_w_str(&w, "protocol");
    op_w_str(&w, protocol);
    op_w_str(&w, "response");
    op_w_str(&w, topic);
    op_w_str(&w, "id");
    op_w_int64(&w, id);
    op_w_str(&w, "status");
    op_w_int64(&w, status);
    if (w.overflow)
        return -1;
    if (!msg) {
        msg = kEmptyDict;
        msgLen = sizeof kEmptyDict;
    }
    return SendFrame(c, hb, w.len, msg, msgLen);
}

int hds_send_event(hds_conn_t* c, const char* protocol, const char* topic, const uint8_t* msg, size_t msgLen) {
    uint8_t hb[128];
    op_writer_t w;
    op_w_init(&w, hb, sizeof hb);
    op_w_dict(&w, 2);
    op_w_str(&w, "protocol");
    op_w_str(&w, protocol);
    op_w_str(&w, "event");
    op_w_str(&w, topic);
    if (w.overflow)
        return -1;
    if (!msg) {
        msg = kEmptyDict;
        msgLen = sizeof kEmptyDict;
    }
    return SendFrame(c, hb, w.len, msg, msgLen);
}

int hds_send_request(hds_conn_t* c, const char* protocol, const char* topic, int64_t id, const uint8_t* msg,
                     size_t msgLen) {
    uint8_t hb[160];
    op_writer_t w;
    op_w_init(&w, hb, sizeof hb);
    op_w_dict(&w, 3);
    op_w_str(&w, "protocol");
    op_w_str(&w, protocol);
    op_w_str(&w, "request");
    op_w_str(&w, topic);
    op_w_str(&w, "id");
    op_w_int64(&w, id);
    if (w.overflow)
        return -1;
    if (!msg) {
        msg = kEmptyDict;
        msgLen = sizeof kEmptyDict;
    }
    return SendFrame(c, hb, w.len, msg, msgLen);
}

void hds_close(hds_conn_t* c) {
    pthread_mutex_lock(&hds.lock);
    if (c->used)
        c->closing = true;
    pthread_mutex_unlock(&hds.lock);
    Wake();
}

// ---------------------------------------------------------------------------------------------------------------------
// Connections (HDS thread)

static void CloseConn(hds_conn_t* c, const char* why) {
    if (!c->used)
        return;
    HAPLogInfo(&logObject, "HDS connection %d closed (%s).", c->fd, why);
    if (c->ready && hds.onClosed)
        hds.onClosed(c);
    pthread_mutex_lock(&c->txLock);
    // Graceful close (as the ADK's HAPPlatformTCPStreamCloseOutput): FIN after what's already queued, then read
    // away what the controller sent but we didn't read. Linux answers close() with unread received data by RST,
    // which discards our unsent tail (e.g. a final dataSend/close).
    (void) shutdown(c->fd, SHUT_WR);
    uint8_t drain[512];
    int guard = 64;
    while (guard-- > 0 && recv(c->fd, drain, sizeof drain, MSG_DONTWAIT) > 0) {
    }
    close(c->fd);
    c->fd = -1;
    c->used = false;
    pthread_mutex_unlock(&c->txLock);
    free(c->rx);
    c->rx = NULL;
    c->rxLen = 0;
    memset(c->c2aKey, 0, sizeof c->c2aKey);
    memset(c->a2cKey, 0, sizeof c->a2cKey);
}

// Tries the prepared sessions' keys on the first frame (nonce 0). Returns true and takes over the keys on a match.
static bool MatchPrepared(hds_conn_t* c, const uint8_t* frame, size_t payloadLen, uint8_t* plain) {
    uint8_t n[8];
    Nonce(0, n);
    bool ok = false;
    pthread_mutex_lock(&hds.lock);
    for (int i = 0; i < MAX_PREPARED && !ok; i++) {
        prepared_t* p = &hds.prepared[i];
        if (!p->used)
            continue;
        if (HAP_chacha20_poly1305_decrypt_aad(frame + 4 + payloadLen, plain, frame + 4, payloadLen, frame, 4, n,
                                              sizeof n, p->c2aKey) == 0) {
            memcpy(c->c2aKey, p->c2aKey, 32);
            memcpy(c->a2cKey, p->a2cKey, 32);
            c->hapSession = p->hapSession;
            memset(p, 0, sizeof *p);
            ok = true;
        }
    }
    pthread_mutex_unlock(&hds.lock);
    return ok;
}

static void HandlePayload(hds_conn_t* c, const uint8_t* plain, size_t len) {
    if (len < 1 || plain[0] > len - 1) {
        CloseConn(c, "bad payload header length");
        return;
    }
    static op_arena_t hdrArena, msgArena; // HDS thread only
    hdrArena.used = hdrArena.numTracked = 0;
    msgArena.used = msgArena.numTracked = 0;
    size_t hl = plain[0];
    const op_val_t* hdr = op_decode(&hdrArena, plain + 1, hl, NULL);
    const op_val_t* msg = len - 1 - hl ? op_decode(&msgArena, plain + 1 + hl, len - 1 - hl, NULL) : NULL;
    if (!hdr || hdr->type != OP_DICT) {
        CloseConn(c, "undecodable header");
        return;
    }
    char protocol[32] = "", topic[48] = "";
    op_str(op_get(hdr, "protocol"), protocol, sizeof protocol);
    const op_val_t* t;
    bool isRequest = false, isResponse = false;
    if ((t = op_get(hdr, "event"))) {
    } else if ((t = op_get(hdr, "request"))) {
        isRequest = true;
    } else if ((t = op_get(hdr, "response"))) {
        isResponse = true;
    } else {
        CloseConn(c, "unknown message type");
        return;
    }
    op_str(t, topic, sizeof topic);
    const op_val_t* idv = op_get(hdr, "id");
    int64_t id = idv && idv->type == OP_INT ? idv->i : 0;

    if (!c->ready) {
        if (!isRequest || strcmp(protocol, "control") || strcmp(topic, "hello")) {
            CloseConn(c, "first message was not control/hello");
            return;
        }
        c->ready = true;
        HAPLogInfo(&logObject, "HDS connection %d: hello, ready.", c->fd);
        (void) hds_send_response(c, "control", "hello", id, kHDSStatus_Success, NULL, 0);
        return;
    }
    if (isResponse) {
        const op_val_t* st = op_get(hdr, "status");
        HAPLogDebug(&logObject, "HDS response %s/%s id %lld status %lld", protocol, topic, (long long) id,
                    st && st->type == OP_INT ? (long long) st->i : -1LL);
    }
    bool handled = hds.onMessage && hds.onMessage(c, protocol, topic, isRequest, id, msg);
    if (isRequest && !handled) {
        HAPLogInfo(&logObject, "HDS request %s/%s not handled: MissingProtocol.", protocol, topic);
        (void) hds_send_response(c, protocol, topic, id, kHDSStatus_MissingProtocol, NULL, 0);
    }
}

static void HandleReadable(hds_conn_t* c) {
    uint8_t buf[4096];
    ssize_t n = recv(c->fd, buf, sizeof buf, 0);
    if (n == 0) {
        CloseConn(c, "peer closed");
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR)
            CloseConn(c, "read error");
        return;
    }
    if (c->rxLen + (size_t) n > RX_MAX) {
        CloseConn(c, "frame too large");
        return;
    }
    if (!c->rx && !(c->rx = malloc(RX_MAX))) {
        CloseConn(c, "out of memory");
        return;
    }
    memcpy(c->rx + c->rxLen, buf, (size_t) n);
    c->rxLen += (size_t) n;

    while (c->used && c->rxLen >= 4) {
        if (c->rx[0] != 1) {
            CloseConn(c, "unknown frame type");
            return;
        }
        size_t payloadLen = ((size_t) c->rx[1] << 16) | ((size_t) c->rx[2] << 8) | c->rx[3];
        size_t frameLen = 4 + payloadLen + 16;
        if (c->rxLen < frameLen)
            return;
        uint8_t* plain = malloc(payloadLen ? payloadLen : 1);
        if (!plain) {
            CloseConn(c, "out of memory");
            return;
        }
        bool ok;
        if (!c->keyed) {
            ok = MatchPrepared(c, c->rx, payloadLen, plain);
            if (ok) {
                c->keyed = true;
                c->c2aNonce = 1;
            }
        } else {
            uint8_t nn[8];
            Nonce(c->c2aNonce, nn);
            ok = HAP_chacha20_poly1305_decrypt_aad(c->rx + 4 + payloadLen, plain, c->rx + 4, payloadLen, c->rx, 4,
                                                   nn, sizeof nn, c->c2aKey) == 0;
            if (ok)
                c->c2aNonce++;
        }
        if (!ok) {
            free(plain);
            CloseConn(c, c->keyed ? "frame authentication failed" : "no prepared session matches");
            return;
        }
        HandlePayload(c, plain, payloadLen);
        free(plain);
        if (!c->used)
            return;
        memmove(c->rx, c->rx + frameLen, c->rxLen - frameLen);
        c->rxLen -= frameLen;
    }
}

static void Accept(void) {
    int fd = accept(hds.listenFd, NULL, NULL);
    if (fd < 0)
        return;
    int one = 1;
    (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    (void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
    for (int i = 0; i < MAX_CONNS; i++) {
        hds_conn_t* c = &hds.conns[i];
        if (c->used)
            continue;
        pthread_mutex_lock(&c->txLock);
        c->used = true;
        c->ready = c->closing = c->keyed = false;
        c->fd = fd;
        c->hapSession = NULL;
        c->c2aNonce = c->a2cNonce = 0;
        c->rxLen = 0;
        c->helloDeadline = NowMs() + HELLO_TIMEOUT_MS;
        pthread_mutex_unlock(&c->txLock);
        HAPLogInfo(&logObject, "HDS connection %d accepted.", fd);
        return;
    }
    HAPLogError(&logObject, "HDS: too many connections, refusing one.");
    close(fd);
}

static void* Thread(void* arg HAP_UNUSED) {
    for (;;) {
        struct pollfd pfd[2 + MAX_CONNS];
        hds_conn_t* map[2 + MAX_CONNS];
        int n = 0;
        pfd[n] = (struct pollfd) { .fd = hds.wakePipe[0], .events = POLLIN };
        map[n++] = NULL;
        pfd[n] = (struct pollfd) { .fd = hds.listenFd, .events = POLLIN };
        map[n++] = NULL;
        uint64_t now = NowMs();
        for (int i = 0; i < MAX_CONNS; i++) {
            hds_conn_t* c = &hds.conns[i];
            if (!c->used)
                continue;
            pthread_mutex_lock(&hds.lock);
            bool closing = c->closing;
            pthread_mutex_unlock(&hds.lock);
            if (closing) {
                CloseConn(c, "closed by rhkd");
                continue;
            }
            if (!c->ready && now > c->helloDeadline) {
                CloseConn(c, "no hello in time");
                continue;
            }
            pfd[n] = (struct pollfd) { .fd = c->fd, .events = POLLIN };
            map[n++] = c;
        }
        pthread_mutex_lock(&hds.lock);
        for (int i = 0; i < MAX_PREPARED; i++)
            if (hds.prepared[i].used && now > hds.prepared[i].deadline) {
                HAPLogInfo(&logObject, "Prepared HDS session expired (controller never connected).");
                memset(&hds.prepared[i], 0, sizeof hds.prepared[i]);
            }
        pthread_mutex_unlock(&hds.lock);

        if (poll(pfd, (nfds_t) n, 1000) <= 0)
            continue;
        if (pfd[0].revents & POLLIN) {
            uint8_t drain[16];
            (void) !read(hds.wakePipe[0], drain, sizeof drain);
        }
        if (pfd[1].revents & POLLIN)
            Accept();
        for (int i = 2; i < n; i++)
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
                HandleReadable(map[i]);
    }
    return NULL;
}

static bool Listen(void) {
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool v6 = fd >= 0;
    if (!v6)
        fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    int one = 1, zero = 0;
    (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int r;
    if (v6) {
        (void) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_addr = in6addr_any };
        r = bind(fd, (struct sockaddr*) &a, sizeof a);
    } else {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
        r = bind(fd, (struct sockaddr*) &a, sizeof a);
    }
    if (r || listen(fd, 4)) {
        close(fd);
        return false;
    }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    getsockname(fd, (struct sockaddr*) &ss, &sl);
    hds.port = ntohs(ss.ss_family == AF_INET6 ? ((struct sockaddr_in6*) &ss)->sin6_port
                                              : ((struct sockaddr_in*) &ss)->sin_port);
    hds.listenFd = fd;
    return true;
}

void hds_init(hds_message_fn onMessage, hds_closed_fn onClosed) {
    hds.onMessage = onMessage;
    hds.onClosed = onClosed;
    for (int i = 0; i < MAX_CONNS; i++) {
        pthread_mutex_init(&hds.conns[i].txLock, NULL);
        hds.conns[i].fd = -1;
    }
    if (pipe(hds.wakePipe) || !Listen()) {
        HAPLogError(&logObject, "HDS: could not create the listening socket; HomeKit Secure Video is unavailable.");
        return;
    }
    (void) fcntl(hds.wakePipe[0], F_SETFL, O_NONBLOCK);
    (void) fcntl(hds.wakePipe[1], F_SETFL, O_NONBLOCK);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, Thread, NULL))
        HAPLogError(&logObject, "HDS: could not start the thread.");
    pthread_attr_destroy(&attr);
    HAPLogInfo(&logObject, "HDS listening on TCP port %u.", hds.port);
}

void hds_session_invalidated(HAPSessionRef* session) {
    bool any = false;
    pthread_mutex_lock(&hds.lock);
    for (int i = 0; i < MAX_PREPARED; i++)
        if (hds.prepared[i].used && hds.prepared[i].hapSession == session)
            memset(&hds.prepared[i], 0, sizeof hds.prepared[i]);
    for (int i = 0; i < MAX_CONNS; i++)
        if (hds.conns[i].used && hds.conns[i].hapSession == session) {
            hds.conns[i].closing = true;
            any = true;
        }
    pthread_mutex_unlock(&hds.lock);
    if (any)
        Wake();
}

// ---------------------------------------------------------------------------------------------------------------------
// Data Stream Transport Management service (HAP run loop)

static const HAPUUID kServiceType_DataStreamTransportManagement = HAPUUIDCreateAppleDefined(0x129);
static const HAPUUID kCharType_SupportedDataStreamTransportConfiguration = HAPUUIDCreateAppleDefined(0x130);
static const HAPUUID kCharType_SetupDataStreamTransport = HAPUUIDCreateAppleDefined(0x131);

static HAPError HandleSupportedRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    // {01 transfer transport configuration {01 transport type = 0 (HomeKit Data Stream over TCP)}}, as the G100.
    const uint8_t inner[] = { 0x01, 0x01, 0x00 };
    return HAPTLVWriterAppend(w, &(const HAPTLV) { .type = 0x01, .value = { .bytes = inner, .numBytes = sizeof inner } });
}

static HAPError HandleSetupRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicReadRequest* request HAP_UNUSED,
        HAPTLVWriterRef* w,
        void* _Nullable context HAP_UNUSED) {
    // The read right after a write (write response) carries the accessory salt; later reads don't.
    size_t len = hds.responseFullOnce ? hds.responseLen : hds.responseLenNoSalt;
    hds.responseFullOnce = false;
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t t = hds.response[pos], l = hds.response[pos + 1];
        HAPError err = HAPTLVWriterAppend(
                w, &(const HAPTLV) { .type = t, .value = { .bytes = &hds.response[pos + 2], .numBytes = l } });
        if (err)
            return err;
        pos += 2u + l;
    }
    return kHAPError_None;
}

static void SetResponse(uint8_t status, const uint8_t* salt) {
    tlvb_t b;
    tlvb_init(&b, hds.response, sizeof hds.response);
    tlvb_put(&b, 0x01, &status, 1);
    if (status == 0) {
        uint8_t port[] = { 0x01, 0x02, (uint8_t) hds.port, (uint8_t) (hds.port >> 8) };
        tlvb_put(&b, 0x02, port, sizeof port);
    }
    hds.responseLenNoSalt = b.len;
    if (salt)
        tlvb_put(&b, 0x03, salt, 32);
    hds.responseLen = b.len;
    hds.responseFullOnce = true;
}

static HAPError HandleSetupWrite(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPTLV8CharacteristicWriteRequest* request,
        HAPTLVReaderRef* r,
        void* _Nullable context HAP_UNUSED) {
    uint8_t cmd = 0xFF, transport = 0xFF, controllerSalt[32];
    size_t saltLen = 0;
    for (;;) {
        bool found;
        HAPTLV tlv;
        if (HAPTLVReaderGetNext(r, &found, &tlv))
            return kHAPError_InvalidData;
        if (!found)
            break;
        if (tlv.type == 0x01 && tlv.value.numBytes == 1)
            cmd = ((const uint8_t*) tlv.value.bytes)[0];
        else if (tlv.type == 0x02 && tlv.value.numBytes == 1)
            transport = ((const uint8_t*) tlv.value.bytes)[0];
        else if (tlv.type == 0x03 && tlv.value.numBytes == sizeof controllerSalt) {
            memcpy(controllerSalt, tlv.value.bytes, sizeof controllerSalt);
            saltLen = sizeof controllerSalt;
        }
    }
    HAPLogInfo(&logObject, "SetupDataStreamTransport: command %u, transport %u.", cmd, transport);
    if (cmd != 0 || transport != 0 || saltLen != 32)
        return kHAPError_InvalidData;
    if (hds.listenFd < 0) {
        SetResponse(1, NULL); // generic error
        return kHAPError_None;
    }

    HAPSession* session = (HAPSession*) request->session;
    uint8_t accessorySalt[32], salt[64];
    HAPPlatformRandomNumberFill(accessorySalt, sizeof accessorySalt);
    memcpy(salt, controllerSalt, 32);
    memcpy(salt + 32, accessorySalt, 32);

    pthread_mutex_lock(&hds.lock);
    prepared_t* p = NULL;
    for (int i = 0; i < MAX_PREPARED && !p; i++)
        if (!hds.prepared[i].used)
            p = &hds.prepared[i];
    if (p) {
        static const char kRead[] = "HDS-Read-Encryption-Key", kWrite[] = "HDS-Write-Encryption-Key";
        HAP_hkdf_sha512(p->a2cKey, 32, session->hap.cv_KEY, sizeof session->hap.cv_KEY, salt, sizeof salt,
                        (const uint8_t*) kRead, sizeof kRead - 1);
        HAP_hkdf_sha512(p->c2aKey, 32, session->hap.cv_KEY, sizeof session->hap.cv_KEY, salt, sizeof salt,
                        (const uint8_t*) kWrite, sizeof kWrite - 1);
        p->hapSession = request->session;
        p->deadline = NowMs() + CONNECT_TIMEOUT_MS;
        p->used = true;
    }
    pthread_mutex_unlock(&hds.lock);
    if (!p) {
        HAPLogError(&logObject, "SetupDataStreamTransport: busy (too many pending sessions).");
        SetResponse(2, NULL); // busy
        return kHAPError_None;
    }
    SetResponse(0, accessorySalt);
    HAPLogInfo(&logObject, "HDS session prepared; controller to connect to port %u.", hds.port);
    return kHAPError_None;
}

static HAPError HandleVersionRead(
        HAPAccessoryServerRef* server HAP_UNUSED,
        const HAPStringCharacteristicReadRequest* request HAP_UNUSED,
        char* value,
        size_t maxValueBytes,
        void* _Nullable context HAP_UNUSED) {
    static const char kVersion[] = "1.0";
    if (maxValueBytes < sizeof kVersion)
        return kHAPError_OutOfResources;
    memcpy(value, kVersion, sizeof kVersion);
    return kHAPError_None;
}

#define kIID_DataStream          ((uint64_t) 0x0070)
#define kIID_DataStreamSupported ((uint64_t) 0x0071)
#define kIID_DataStreamSetup     ((uint64_t) 0x0072)
#define kIID_DataStreamVersion   ((uint64_t) 0x0073)

#define PROPS(r, w, ev, wr) \
    { .readable = r, \
      .writable = w, \
      .supportsEventNotification = ev, \
      .hidden = false, \
      .requiresTimedWrite = false, \
      .supportsAuthorizationData = false, \
      .ip = { .controlPoint = false, .supportsWriteResponse = wr }, \
      .ble = { .supportsBroadcastNotification = false, \
               .supportsDisconnectedNotification = false, \
               .readableWithoutSecurity = false, \
               .writableWithoutSecurity = false } }

static const HAPTLV8Characteristic supportedCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_DataStreamSupported,
    .characteristicType = &kCharType_SupportedDataStreamTransportConfiguration,
    .debugDescription = "supported-data-stream-transport-configuration",
    .properties = PROPS(true, false, false, false),
    .callbacks = { .handleRead = HandleSupportedRead },
};

static const HAPTLV8Characteristic setupCharacteristic = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_DataStreamSetup,
    .characteristicType = &kCharType_SetupDataStreamTransport,
    .debugDescription = "setup-data-stream-transport",
    .properties = PROPS(true, true, false, true),
    .callbacks = { .handleRead = HandleSetupRead, .handleWrite = HandleSetupWrite },
};

static const HAPStringCharacteristic versionCharacteristic = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_DataStreamVersion,
    .characteristicType = &kHAPCharacteristicType_Version,
    .debugDescription = kHAPCharacteristicDebugDescription_Version,
    .properties = PROPS(true, false, false, false),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HandleVersionRead },
};

const HAPService dataStreamService = {
    .iid = kIID_DataStream,
    .serviceType = &kServiceType_DataStreamTransportManagement,
    .debugDescription = "data-stream-transport-management",
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &supportedCharacteristic,
                                                            &setupCharacteristic,
                                                            &versionCharacteristic,
                                                            NULL },
};

#include "bridge.h"

// winsock2.h must precede windows.h; CMake defines WIN32_LEAN_AND_MEAN so
// windows.h does not drag the old winsock.h in behind it.
#include <winsock2.h>
#include <windows.h>

#include <cstring>

#include "lk2_mailbox.h"
#include "lk2_proto.h"
#include "log.h"

namespace k2se {
namespace bridge {
namespace {

using lk2::Mailbox;
using lk2::Message;

// --- Winsock, resolved at run time --------------------------------------------
// Only the function POINTERS come from ws2_32.dll; the types and constants are
// the header's. FD_ISSET is avoided because its macro calls __WSAFDIsSet, which
// would be an import: with one socket in the set, select()'s return value says
// everything.
struct Ws {
    HMODULE lib = nullptr;
    decltype(&WSAStartup) startup = nullptr;
    decltype(&WSACleanup) cleanup = nullptr;
    decltype(&WSAGetLastError) lastError = nullptr;
    decltype(&socket) socket = nullptr;
    decltype(&connect) connect = nullptr;
    decltype(&send) send = nullptr;
    decltype(&recv) recv = nullptr;
    decltype(&select) select = nullptr;
    decltype(&setsockopt) setsockopt = nullptr;
    decltype(&shutdown) shutdown = nullptr;
    decltype(&closesocket) closesocket = nullptr;
};

struct State {
    bool started = false;
    HANDLE thread = nullptr;
    volatile LONG stop = 0;
    CRITICAL_SECTION lock;
    bool lockReady = false;
    Ws ws;
    bool wsaReady = false;
    SOCKET sock = INVALID_SOCKET;
    volatile LONG connected = 0;
    uint32_t sent = 0, received = 0, dropped = 0, lost = 0;
    bool connectFailLogged = false;
    bool queueFullLogged = false;
};

Mailbox g_box;  // ~260 KB of static storage; never on a stack
State g_st;

int WsaError() { return g_st.ws.lastError ? g_st.ws.lastError() : -1; }

template <typename Fn>
bool Resolve(Fn* out, const char* name) {
    *out = reinterpret_cast<Fn>(GetProcAddress(g_st.ws.lib, name));
    if (!*out) log::Writef("bridge: ws2_32!%s not found", name);
    return *out != nullptr;
}

bool LoadWinsock() {
    Ws& w = g_st.ws;
    w.lib = LoadLibraryA("ws2_32.dll");
    if (!w.lib) {
        log::Writef("bridge: LoadLibrary(ws2_32.dll) failed (error %u)", GetLastError());
        return false;
    }
    const bool ok = Resolve(&w.startup, "WSAStartup") && Resolve(&w.cleanup, "WSACleanup") &&
                    Resolve(&w.lastError, "WSAGetLastError") && Resolve(&w.socket, "socket") &&
                    Resolve(&w.connect, "connect") && Resolve(&w.send, "send") &&
                    Resolve(&w.recv, "recv") && Resolve(&w.select, "select") &&
                    Resolve(&w.setsockopt, "setsockopt") && Resolve(&w.shutdown, "shutdown") &&
                    Resolve(&w.closesocket, "closesocket");
    if (!ok) return false;
    WSADATA data;
    const int rc = w.startup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        log::Writef("bridge: WSAStartup failed (%d)", rc);
        return false;
    }
    g_st.wsaReady = true;
    return true;
}

void Disconnect(const char* why) {
    if (g_st.sock == INVALID_SOCKET) return;
    g_st.ws.shutdown(g_st.sock, SD_BOTH);
    g_st.ws.closesocket(g_st.sock);
    g_st.sock = INVALID_SOCKET;
    InterlockedExchange(&g_st.connected, 0);
    log::Writef("bridge: disconnected (%s, WSA %d); reconnecting", why, WsaError());
}

bool Connect() {
    Ws& w = g_st.ws;
    SOCKET s = w.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (!g_st.connectFailLogged) log::Writef("bridge: socket() failed (WSA %d)", WsaError());
        g_st.connectFailLogged = true;
        return false;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    // htons/inet_addr are ws2_32 imports too; both are one line of arithmetic.
    addr.sin_port = static_cast<u_short>(((kPort & 0xFF) << 8) | ((kPort >> 8) & 0xFF));
    addr.sin_addr.s_addr = 0x0100007F;  // 127.0.0.1, network byte order
    if (w.connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        const int err = WsaError();
        w.closesocket(s);
        if (!g_st.connectFailLogged)
            log::Writef("bridge: connect to 127.0.0.1:%d failed (WSA %d); retrying every 1 s, "
                        "silently until it changes", kPort, err);
        g_st.connectFailLogged = true;
        return false;
    }
    const BOOL on = TRUE;
    w.setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    g_st.sock = s;
    g_st.connectFailLogged = false;
    InterlockedExchange(&g_st.connected, 1);
    log::Writef("bridge: connected to 127.0.0.1:%d", kPort);
    return true;
}

bool SendAll(const char* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        const int n = g_st.ws.send(g_st.sock, data + done, static_cast<int>(len - done), 0);
        if (n == SOCKET_ERROR || n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

void HandleLine(const char* line, size_t len) {
    static char text[lk2::kMaxText];  // worker thread only
    int id = 0;
    uint32_t n = 0;
    bool cut = false;
    if (!lk2::DecodeLine(line, len, &id, text, sizeof(text), &n, &cut)) {
        log::Writef("bridge: undecodable line (%u bytes) \"%.60s\"", static_cast<unsigned>(len), line);
        ++g_st.dropped;
        return;
    }
    if (cut) log::Writef("bridge: reply id=%d cut to %u bytes", id, n);

    EnterCriticalSection(&g_st.lock);
    const int latest = g_box.LastPostedId();
    bool cut2 = false;
    const Mailbox::Deliver d = g_box.DeliverReply(id, text, n, &cut2);
    LeaveCriticalSection(&g_st.lock);

    switch (d) {
        case Mailbox::Deliver::kStored:
            ++g_st.received;
            log::Writef("bridge: reply id=%d len=%u stored \"%.80s\"", id, n, text);
            break;
        case Mailbox::Deliver::kLate:
            ++g_st.dropped;
            log::Writef("bridge: LATE reply id=%d dropped (latest post is %d) \"%.80s\"", id,
                        latest, text);
            break;
        case Mailbox::Deliver::kUnknown:
            ++g_st.dropped;
            log::Writef("bridge: reply id=%d dropped: no such request (latest post is %d)", id,
                        latest);
            break;
        case Mailbox::Deliver::kFull:
            ++g_st.dropped;
            log::Writef("bridge: reply id=%d dropped: inbound table full", id);
            break;
    }
}

DWORD WINAPI Worker(LPVOID) {
    if (!LoadWinsock()) {
        log::Write("bridge: worker exiting; LK2_Poll will always return \"\"");
        return 0;
    }
    log::Writef("bridge: worker running; target 127.0.0.1:%d", kPort);

    static char rx[2 * lk2::kMaxText + 64];
    size_t rxLen = 0;
    static char line[lk2::kMaxText + 64];

    while (!InterlockedCompareExchange(&g_st.stop, 0, 0)) {
        if (g_st.sock == INVALID_SOCKET) {
            if (!Connect()) {
                Sleep(1000);
                continue;
            }
            rxLen = 0;
        }

        // Everything the game queued since the last pass.
        for (;;) {
            Message msg;
            EnterCriticalSection(&g_st.lock);
            const bool have = g_box.TakeOutbound(&msg);
            LeaveCriticalSection(&g_st.lock);
            if (!have) break;
            const size_t n = lk2::EncodeLine(msg.id, msg.text, msg.len, line, sizeof(line));
            if (n == 0) {
                log::Writef("bridge: request id=%d could not be encoded (%u bytes); lost", msg.id,
                            msg.len);
                ++g_st.lost;
                continue;
            }
            if (!SendAll(line, n)) {
                log::Writef("bridge: request id=%d lost in send", msg.id);
                ++g_st.lost;
                Disconnect("send failed");
                break;
            }
            ++g_st.sent;
            log::Writef("bridge: sent id=%d len=%u \"%.80s\"", msg.id, msg.len, msg.text);
        }
        if (g_st.sock == INVALID_SOCKET) continue;

        // Up to 50 ms for a reply, then back to the outbound queue.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(g_st.sock, &readable);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        const int ready = g_st.ws.select(0, &readable, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) {
            Disconnect("select failed");
            continue;
        }
        if (ready == 0) continue;

        const int got = g_st.ws.recv(g_st.sock, rx + rxLen, static_cast<int>(sizeof(rx) - rxLen), 0);
        if (got == 0) {
            Disconnect("peer closed");
            continue;
        }
        if (got < 0) {
            Disconnect("recv failed");
            continue;
        }
        rxLen += static_cast<size_t>(got);

        size_t start = 0;
        for (size_t i = 0; i < rxLen; ++i) {
            if (rx[i] != '\n') continue;
            HandleLine(rx + start, i - start);
            start = i + 1;
        }
        if (start) {
            std::memmove(rx, rx + start, rxLen - start);
            rxLen -= start;
        }
        if (rxLen == sizeof(rx)) {
            log::Writef("bridge: inbound line longer than %u bytes; discarded",
                        static_cast<unsigned>(sizeof(rx)));
            rxLen = 0;
        }
    }

    Disconnect("stopping");
    if (g_st.wsaReady) g_st.ws.cleanup();
    return 0;
}

// Game thread. The lock exists before the thread that will contend for it.
void EnsureStarted() {
    if (g_st.started) return;
    g_st.started = true;
    InitializeCriticalSection(&g_st.lock);
    g_st.lockReady = true;
    g_st.stop = 0;
    g_st.thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_st.thread) {
        log::Writef("bridge: worker thread could not start (error %u)", GetLastError());
        return;
    }
    log::Writef("bridge: worker started on first use (port %d)", kPort);
}

}  // namespace

int Post(const char* text, uint32_t len) {
    EnsureStarted();
    if (!g_st.lockReady) return 0;
    EnterCriticalSection(&g_st.lock);
    const int id = g_box.Post(text, len);
    const int queued = g_box.OutboundCount();
    LeaveCriticalSection(&g_st.lock);
    if (id == 0) {
        if (!g_st.queueFullLogged)
            log::Writef("bridge: outbound queue full (%d queued; responder absent?) -> "
                        "LK2_Post returns 0 until it drains", queued);
        g_st.queueFullLogged = true;
        return 0;
    }
    g_st.queueFullLogged = false;
    log::Writef("bridge: post id=%d len=%u \"%.80s\" (queued %d, %s)", id, len, text, queued,
                InterlockedCompareExchange(&g_st.connected, 0, 0) ? "connected" : "not connected");
    return id;
}

bool Poll(int id, char* out, uint32_t cap, uint32_t* len) {
    EnsureStarted();
    if (!g_st.lockReady) return false;
    EnterCriticalSection(&g_st.lock);
    const bool ok = g_box.Poll(id, out, cap, len);
    const int latest = g_box.LastPostedId();
    LeaveCriticalSection(&g_st.lock);
    if (ok) {
        log::Writef("bridge: poll id=%d (latest %d) -> len=%u \"%.80s\"", id, latest, *len, out);
    } else {
        log::Trace("bridge: poll id=%d (latest %d) -> nothing", id, latest);
    }
    return ok;
}

void Remove() {
    if (!g_st.started) return;
    InterlockedExchange(&g_st.stop, 1);
    bool workerGone = true;
    if (g_st.thread) {
        // At process exit the worker is already dead (log.h explains the order),
        // so this returns at once; under an explicit unload it ends within one
        // retry sleep. Either way the wait is bounded.
        workerGone = WaitForSingleObject(g_st.thread, 1500) == WAIT_OBJECT_0;
        CloseHandle(g_st.thread);
        g_st.thread = nullptr;
    }
    if (g_st.lockReady && workerGone) {
        DeleteCriticalSection(&g_st.lock);
        g_st.lockReady = false;
    }
    log::Writef("bridge: removed; sent %u, received %u, dropped %u, lost %u%s", g_st.sent,
                g_st.received, g_st.dropped, g_st.lost,
                workerGone ? "" : " (worker did not stop in time)");
    g_st.started = false;
}

int Status() {
    int s = g_st.started ? 1 : 0;
    if (InterlockedCompareExchange(&g_st.connected, 0, 0)) s |= 2;
    return s;
}

}  // namespace bridge
}  // namespace k2se

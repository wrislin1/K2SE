#include "bridge.h"

// winsock2.h must precede windows.h; CMake defines WIN32_LEAN_AND_MEAN so
// windows.h does not drag the old winsock.h in behind it.
#include <winsock2.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "lk2_mailbox.h"
#include "lk2_meter.h"
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
    // Game-thread meters (Stage 3, E4 as a number). Post/Poll run on the game
    // thread only; Remove() reads them after the worker has stopped.
    uint64_t qpcFreq = 0;
    lk2::MaxTimer postTimer;
    lk2::MaxTimer pollTimer;
    // Stage 4 run 8 printed poll=2684 us and could not say why: pollTimer spans
    // the diagnostic log write too. These split it: the locked mailbox section,
    // and the log call alone (only polls that made one).
    lk2::MaxTimer pollLockTimer;
    lk2::MaxTimer pollLogTimer;
    lk2::PollTraceThinner thinner;
};

Mailbox g_box;  // ~260 KB of static storage; never on a stack
State g_st;

// Every bridge line carries "t=<ms since the DLL loaded>" so post -> stored
// latency can be read DLL-side; the responder log has the wall clock and the
// two are aligned on the "connected" line.
void BLog(const char* fmt, ...) {
    char body[900];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';
    log::Writef("bridge: t=%u %s", log::MillisSinceInit(), body);
}

void BTrace(const char* fmt, ...) {
    if (!log::DiagnosticEnabled()) return;
    char body[900];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    body[sizeof(body) - 1] = '\0';
    log::Trace("bridge: t=%u %s", log::MillisSinceInit(), body);
}

uint64_t Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

// Records the time from construction to scope exit into a MaxTimer, tagged
// with the log's t= at entry: one of these at the top of Post/Poll covers every
// return path, log write included.
struct ScopedMax {
    lk2::MaxTimer& timer;
    uint32_t at;
    uint64_t t0;
    explicit ScopedMax(lk2::MaxTimer& t) : timer(t), at(log::MillisSinceInit()), t0(Now()) {}
    ~ScopedMax() { timer.Record(t0, Now(), at); }
};

int WsaError() { return g_st.ws.lastError ? g_st.ws.lastError() : -1; }

template <typename Fn>
bool Resolve(Fn* out, const char* name) {
    *out = reinterpret_cast<Fn>(GetProcAddress(g_st.ws.lib, name));
    if (!*out) BLog("ws2_32!%s not found", name);
    return *out != nullptr;
}

bool LoadWinsock() {
    Ws& w = g_st.ws;
    w.lib = LoadLibraryA("ws2_32.dll");
    if (!w.lib) {
        BLog("LoadLibrary(ws2_32.dll) failed (error %u)", GetLastError());
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
        BLog("WSAStartup failed (%d)", rc);
        return false;
    }
    g_st.wsaReady = true;
    return true;
}

void Disconnect(const char* why) {
    if (g_st.sock == INVALID_SOCKET) return;
    const int err = WsaError();  // before shutdown/closesocket overwrite it
    g_st.ws.shutdown(g_st.sock, SD_BOTH);
    g_st.ws.closesocket(g_st.sock);
    g_st.sock = INVALID_SOCKET;
    InterlockedExchange(&g_st.connected, 0);
    BLog("disconnected (%s, WSA %d); reconnecting", why, err);
}

bool Connect() {
    Ws& w = g_st.ws;
    SOCKET s = w.socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        if (!g_st.connectFailLogged) BLog("socket() failed (WSA %d)", WsaError());
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
            BLog("connect to 127.0.0.1:%d failed (WSA %d); retrying every 1 s, "
                        "silently until it changes", kPort, err);
        g_st.connectFailLogged = true;
        return false;
    }
    const BOOL on = TRUE;
    w.setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    g_st.sock = s;
    g_st.connectFailLogged = false;
    InterlockedExchange(&g_st.connected, 1);
    BLog("connected to 127.0.0.1:%d", kPort);
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
        BLog("undecodable line (%u bytes) \"%.*s\"", static_cast<unsigned>(len),
             static_cast<int>(len < 60 ? len : 60), line);
        ++g_st.dropped;
        return;
    }
    if (cut) BLog("reply id=%d cut to %u bytes", id, n);

    EnterCriticalSection(&g_st.lock);
    const int latest = g_box.LastPostedId();
    bool cut2 = false;
    const Mailbox::Deliver d = g_box.DeliverReply(id, text, n, &cut2);
    LeaveCriticalSection(&g_st.lock);

    switch (d) {
        case Mailbox::Deliver::kStored:
            ++g_st.received;
            BLog("reply id=%d len=%u stored \"%.80s\"", id, n, text);
            break;
        case Mailbox::Deliver::kLate:
            ++g_st.dropped;
            BLog("LATE reply id=%d dropped (latest post is %d) \"%.80s\"", id,
                        latest, text);
            break;
        case Mailbox::Deliver::kUnknown:
            ++g_st.dropped;
            BLog("reply id=%d dropped: no such request (latest post is %d)", id,
                        latest);
            break;
        case Mailbox::Deliver::kFull:
            ++g_st.dropped;
            BLog("reply id=%d dropped: inbound table full", id);
            break;
    }
}

DWORD WINAPI Worker(LPVOID) {
    if (!LoadWinsock()) {
        BLog("worker exiting; LK2_Poll will always return \"\"");
        return 0;
    }
    BLog("worker running; target 127.0.0.1:%d", kPort);

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
                BLog("request id=%d could not be encoded (%u bytes); lost", msg.id,
                            msg.len);
                ++g_st.lost;
                continue;
            }
            if (!SendAll(line, n)) {
                BLog("request id=%d lost in send", msg.id);
                ++g_st.lost;
                Disconnect("send failed");
                break;
            }
            ++g_st.sent;
            BLog("sent id=%d len=%u \"%.80s\"", msg.id, msg.len, msg.text);
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
            BLog("inbound line longer than %u bytes; discarded",
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
    const uint64_t t0 = Now();
    LARGE_INTEGER f;
    g_st.qpcFreq = QueryPerformanceFrequency(&f) ? static_cast<uint64_t>(f.QuadPart) : 0;
    // The checker's E4 bound: count calls over 1000 us, not just the worst one.
    g_st.postTimer.overTicks = g_st.qpcFreq / 1000;
    g_st.pollTimer.overTicks = g_st.qpcFreq / 1000;
    InitializeCriticalSection(&g_st.lock);
    g_st.lockReady = true;
    g_st.stop = 0;
    g_st.thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_st.thread) {
        BLog("worker thread could not start (error %u)", GetLastError());
        return;
    }
    BLog("worker started on first use (port %d) in %u us", kPort,
         lk2::TicksToMicros(Now() - t0, g_st.qpcFreq));
}

}  // namespace

int Post(const char* text, uint32_t len) {
    EnsureStarted();
    if (!g_st.lockReady) return 0;
    ScopedMax meter(g_st.postTimer);
    bool cut = false;
    EnterCriticalSection(&g_st.lock);
    const int id = g_box.Post(text, len, &cut);
    const int queued = g_box.OutboundCount();
    LeaveCriticalSection(&g_st.lock);
    if (id == 0) {
        if (!g_st.queueFullLogged)
            BLog("outbound queue full (%d queued; responder absent?) -> "
                        "LK2_Post returns 0 until it drains", queued);
        g_st.queueFullLogged = true;
        return 0;
    }
    g_st.queueFullLogged = false;
    if (cut) {
        BLog("post id=%d cut from %u to %u bytes (kMaxText)", id, len, lk2::kMaxText - 1);
        len = lk2::kMaxText - 1;  // the length that was queued and will be sent
    }
    BLog("post id=%d len=%u \"%.80s\" (queued %d, %s)", id, len, text, queued,
                InterlockedCompareExchange(&g_st.connected, 0, 0) ? "connected" : "not connected");
    return id;
}

bool Poll(int id, char* out, uint32_t cap, uint32_t* len) {
    EnsureStarted();
    if (!g_st.lockReady) return false;
    ScopedMax meter(g_st.pollTimer);
    bool ok;
    int latest;
    {
        ScopedMax lockMeter(g_st.pollLockTimer);
        EnterCriticalSection(&g_st.lock);
        ok = g_box.Poll(id, out, cap, len);
        latest = g_box.LastPostedId();
        LeaveCriticalSection(&g_st.lock);
    }
    if (ok) {
        ScopedMax logMeter(g_st.pollLogTimer);
        BLog("poll id=%d (latest %d) -> len=%u \"%.80s\"", id, latest, *len, out);
    } else {
        // One trace per (id, latest) state; run 1 wrote 100 identical lines
        // between "End." and quit.
        uint32_t swallowed = 0;
        if (g_st.thinner.Note(id, latest, &swallowed)) {
            ScopedMax logMeter(g_st.pollLogTimer);
            if (swallowed)
                BTrace("poll id=%d (latest %d) -> nothing [%u earlier identical polls untraced]",
                       id, latest, swallowed);
            else
                BTrace("poll id=%d (latest %d) -> nothing", id, latest);
        }
    }
    return ok;
}

void Remove() {
    if (!g_st.started) return;
    InterlockedExchange(&g_st.stop, 1);
    bool workerGone = true;
    if (g_st.thread) {
        // The worker reads `stop` only at the top of its loop, so an explicit
        // unload waits for whatever it is inside: a 50 ms select when connected,
        // up to the 1 s retry sleep when not, or a connect() that loopback
        // answers at once. 1500 ms covers all three. At process exit the thread
        // is already gone (log.h explains the order) and this returns at once.
        workerGone = WaitForSingleObject(g_st.thread, 1500) == WAIT_OBJECT_0;
        CloseHandle(g_st.thread);
        g_st.thread = nullptr;
    }
    if (g_st.lockReady && workerGone) {
        DeleteCriticalSection(&g_st.lock);
        g_st.lockReady = false;
    }
    BLog("removed; sent %u, received %u, dropped %u, lost %u%s", g_st.sent, g_st.received,
         g_st.dropped, g_st.lost, workerGone ? "" : " (worker did not stop in time)");
    // Exit criterion E4 as a number: the longest the game thread ever spent in
    // either routine, log write included.
    BLog("game-thread max post=%u us poll=%u us over %u posts, %u polls",
         lk2::TicksToMicros(g_st.postTimer.maxTicks, g_st.qpcFreq),
         lk2::TicksToMicros(g_st.pollTimer.maxTicks, g_st.qpcFreq), g_st.postTimer.count,
         g_st.pollTimer.count);
    // Where that time went: when each maximum happened (t= of the call), how
    // many calls broke 1000 us, and the poll split into its locked mailbox
    // section and its log call (a delivered reply always writes a line; an
    // empty-poll trace writes only under K2SE_DIAGNOSTIC).
    BLog("game-thread detail: post max at t=%u, %u over 1000 us; poll max at t=%u, %u over 1000 us; "
         "poll locked max=%u us at t=%u; poll log call max=%u us at t=%u over %u calls",
         g_st.postTimer.maxAt, g_st.postTimer.over, g_st.pollTimer.maxAt, g_st.pollTimer.over,
         lk2::TicksToMicros(g_st.pollLockTimer.maxTicks, g_st.qpcFreq), g_st.pollLockTimer.maxAt,
         lk2::TicksToMicros(g_st.pollLogTimer.maxTicks, g_st.qpcFreq), g_st.pollLogTimer.maxAt,
         g_st.pollLogTimer.count);
    const uint32_t untraced = g_st.thinner.Flush();
    if (untraced) BTrace("%u trailing identical empty polls untraced", untraced);
    g_st.started = false;
}

int Status() {
    int s = g_st.started ? 1 : 0;
    if (InterlockedCompareExchange(&g_st.connected, 0, 0)) s |= 2;
    return s;
}

}  // namespace bridge
}  // namespace k2se

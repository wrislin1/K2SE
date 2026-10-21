// Windows-only integration harness. Compile the actual logger and Poll into
// this TU so fixtures can own their private handles/state without test hooks
// in the DLL. Only the OS/CRT boundary is wrapped; real WriteFile still runs.
#include <winsock2.h>
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

static DWORD g_probeThread = 0;
static DWORD g_writeDelay = 0, g_formatDelay = 0;
static HANDLE g_enterWrite = nullptr, g_releaseWrite = nullptr;

static BOOL WINAPI HarnessWriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w,
                                   LPOVERLAPPED o) {
    if (GetCurrentThreadId() == g_probeThread) {
        if (g_enterWrite) {
            SetEvent(g_enterWrite);
            WaitForSingleObject(g_releaseWrite, 5000);
        }
        if (g_writeDelay) Sleep(g_writeDelay);
    }
    return WriteFile(h, b, n, w, o);
}

static int HarnessVsnprintf(char* b, size_t n, const char* f, va_list a) {
    if (GetCurrentThreadId() == g_probeThread && g_formatDelay) Sleep(g_formatDelay);
    return _vsnprintf(b, n, f, a);
}

#define WriteFile HarnessWriteFile
#define _vsnprintf HarnessVsnprintf
#include "../src/log.cpp"
#include "../src/bridge.cpp"
#undef _vsnprintf
#undef WriteFile

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; \
    std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

using namespace k2se;
using namespace k2se::lk2;
static wchar_t g_path[MAX_PATH];
static uint64_t g_freq = 0;

static void OpenLog(DWORD access = GENERIC_READ | FILE_APPEND_DATA) {
    if (log::g_file != INVALID_HANDLE_VALUE) CloseHandle(log::g_file);
    log::g_file = CreateFileW(g_path, access, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(log::g_file != INVALID_HANDLE_VALUE);
    log::g_diagnostic = true;
}

static std::string ReadLog() {
    SetFilePointer(log::g_file, 0, nullptr, FILE_BEGIN);
    char bytes[16384]; DWORD n = 0;
    CHECK(ReadFile(log::g_file, bytes, sizeof(bytes), &n, nullptr));
    return std::string(bytes, n);
}

static void EmptyLog() {
    if (log::g_file != INVALID_HANDLE_VALUE) CloseHandle(log::g_file);
    HANDLE h = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    log::g_file = INVALID_HANDLE_VALUE;
    OpenLog();
}

static void ResetPoll() {
    if (bridge::g_st.lockReady) DeleteCriticalSection(&bridge::g_st.lock);
    bridge::g_st = bridge::State{};
    bridge::g_st.started = true; // fixture owns initialization; no worker/socket
    bridge::g_st.qpcFreq = g_freq;
    bridge::g_st.pollTimer.overTicks = g_freq / 1000;
    InitializeCriticalSection(&bridge::g_st.lock);
    bridge::g_st.lockReady = true;
}

// Catches byte changes, missing results, and stale fields when a sink is reused.
static void TestLogBytesAndOutcomes() {
    EmptyLog();
    WriteSample s;
    log::Writef("normal %d", 7);
    log::WritefMeasured(s, "normal %d", 7);
    CHECK(s.state == WriteState::Ok); CHECK(s.requested == 10);
    CHECK(s.written == 10); CHECK(s.error == 0);
    CHECK(s.span.end >= s.span.begin); CHECK(s.span.begin != 0);
    log::Trace("trace %d", 9);
    log::TraceMeasured(s, "trace %d", 9);
    CHECK(s.state == WriteState::Ok); CHECK(s.requested == 19);
    CHECK(s.written == 19);
    CHECK(ReadLog() == "normal 7\r\nnormal 7\r\n  [trace] trace 9\r\n  [trace] trace 9\r\n");
    log::g_diagnostic = false;
    const std::string prior = ReadLog();
    log::TraceMeasured(s, "absent");
    CHECK(s.state == WriteState::None); CHECK(s.requested == 0);
    CHECK(s.span.begin == 0); CHECK(s.span.end == 0);
    CHECK(ReadLog() == prior);
    OpenLog(GENERIC_READ);
    log::WritefMeasured(s, "denied");
    CHECK(s.state == WriteState::Failed); CHECK(s.error == ERROR_ACCESS_DENIED);
    CHECK(s.requested == 8); CHECK(s.written == 0);
    CHECK(s.span.end >= s.span.begin); CHECK(ReadLog() == prior);
    CloseHandle(log::g_file); log::g_file = INVALID_HANDLE_VALUE;
    log::WritefMeasured(s, "skipped");
    CHECK(s.state == WriteState::Skipped); CHECK(s.requested == 9);
    CHECK(s.written == 0); CHECK(s.error == 0);
    CHECK(s.span.begin == 0); CHECK(s.span.end == 0);
}

static void CheckNested(const PollSample& s) {
    CHECK(s.ordinal == 1); CHECK(s.id == -1); CHECK(s.latest == 0);
    CHECK(!s.found); CHECK(s.logCalled); CHECK(s.write.state == WriteState::Ok);
    CHECK(s.full.begin <= s.locked.begin);
    CHECK(s.locked.begin <= s.locked.end); CHECK(s.locked.end <= s.log.begin);
    CHECK(s.log.begin <= s.write.span.begin);
    CHECK(s.write.span.begin <= s.write.span.end);
    CHECK(s.write.span.end <= s.log.end); CHECK(s.log.end <= s.full.end);
    CHECK(bridge::g_st.pollTimer.maxTicks == s.full.end - s.full.begin);
    CHECK(bridge::g_st.pollTimer.maxAt == s.at);
}

// Catches moving WriteFile or formatting outside the measured outer scope.
// Sleep exists only in this harness's wrappers, never in the DLL sources.
static void TestDelayedWriteAndFormatting() {
    EmptyLog(); ResetPoll();
    g_writeDelay = 5;
    char reply[32]; uint32_t len = 0;
    CHECK(!bridge::Poll(-1, reply, sizeof(reply), &len));
    g_writeDelay = 0;
    PollSample s = bridge::g_st.worstPoll;
    CheckNested(s);
    CHECK(s.write.span.end - s.write.span.begin >= g_freq / 500); // >= 2 ms
    CHECK(bridge::g_st.pollTimer.over == 1);
    CHECK(bridge::g_st.pollLogTimer.count == 1);
    CHECK(!bridge::Poll(-1, reply, sizeof(reply), &len)); // same state: thinned
    CHECK(bridge::g_st.pollTimer.count == 2);
    CHECK(bridge::g_st.pollLogTimer.count == 1);
    // Do not assume a later thinned call cannot be descheduled longer. Pair
    // selection itself is checked with deterministic ticks in the portable test.
    CHECK(ReadLog().find("poll id=-1 (latest 0) -> nothing") != std::string::npos);
    ResetPoll();
    g_formatDelay = 5;
    CHECK(!bridge::Poll(-1, reply, sizeof(reply), &len));
    g_formatDelay = 0;
    s = bridge::g_st.worstPoll;
    CheckNested(s);
    CHECK(s.write.span.begin - s.log.begin >= g_freq / 500);
    CHECK(bridge::g_st.pollTimer.over == 1);
    bridge::Remove();
    const std::string data = ReadLog();
    CHECK(data.find("game-thread max post=") != std::string::npos);
    CHECK(data.find("game-thread detail:") != std::string::npos);
    CHECK(data.find("game-thread paired: ordinal=1") != std::string::npos);
    CHECK(data.find("game-thread paired write: state=2") != std::string::npos);
}

static DWORD WINAPI WorkerLog(void*) {
    if (WaitForSingleObject(g_enterWrite, 5000) == WAIT_OBJECT_0)
        log::Writef("worker ordinary line with a different length");
    SetEvent(g_releaseWrite);
    return 0;
}

static void TestDeliveredPollUsesMeasuredNormalLog() {
    EmptyLog(); ResetPoll();
    bool cut = false;
    CHECK(bridge::g_box.DeliverReply(0, "resume", 6, &cut) == Mailbox::Deliver::kStored);
    char reply[32]; uint32_t len = 0;
    CHECK(bridge::Poll(0, reply, sizeof(reply), &len));
    CHECK(len == 6); CHECK(std::strcmp(reply, "resume") == 0);
    const PollSample& s = bridge::g_st.worstPoll;
    CHECK(s.ordinal == 1); CHECK(s.id == 0); CHECK(s.found);
    CHECK(s.logCalled); CHECK(s.write.state == WriteState::Ok);
    CHECK(s.log.begin <= s.write.span.begin);
    CHECK(s.write.span.end <= s.log.end); CHECK(s.log.end <= s.full.end);
    const std::string data = ReadLog();
    CHECK(data.find("bridge: ") == 0);
    CHECK(data.find("poll id=0 (latest 0) -> len=6 \"resume\"\r\n") != std::string::npos);
    CHECK(s.write.requested == data.size()); CHECK(s.write.written == data.size());
}

// Force overlap while the measured caller is inside its write boundary.
// Catches a shared/global sink being overwritten by unrelated worker logging.
static void TestOverlappingWorkerCannotReplaceCallerSample() {
    EmptyLog(); ResetPoll();
    g_enterWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_releaseWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(g_enterWrite != nullptr); CHECK(g_releaseWrite != nullptr);
    HANDLE worker = CreateThread(nullptr, 0, WorkerLog, nullptr, 0, nullptr);
    CHECK(worker != nullptr);
    char reply[32]; uint32_t len = 0;
    CHECK(!bridge::Poll(-1, reply, sizeof(reply), &len));
    CHECK(WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0);
    CloseHandle(worker); CloseHandle(g_enterWrite); CloseHandle(g_releaseWrite);
    g_enterWrite = g_releaseWrite = nullptr;
    CheckNested(bridge::g_st.worstPoll);
    const std::string data = ReadLog();
    const char* prefix = "worker ordinary line with a different length\r\n";
    CHECK(data.find(prefix) == 0);
    CHECK(data.find("poll id=-1 (latest 0) -> nothing") != std::string::npos);
    CHECK(bridge::g_st.worstPoll.write.requested == data.size() - std::strlen(prefix));
    CHECK(bridge::g_st.worstPoll.write.written == bridge::g_st.worstPoll.write.requested);
    CHECK(bridge::g_st.pollTimer.count == 1);
}

int main() {
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f)) return 2;
    g_freq = static_cast<uint64_t>(f.QuadPart);
    g_probeThread = GetCurrentThreadId();
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp) || !GetTempFileNameW(tmp, L"lk2", 0, g_path)) return 2;
    TestLogBytesAndOutcomes();
    TestDelayedWriteAndFormatting();
    TestDeliveredPollUsesMeasuredNormalLog();
    TestOverlappingWorkerCannotReplaceCallerSample();
    if (bridge::g_st.lockReady) DeleteCriticalSection(&bridge::g_st.lock);
    if (log::g_file != INVALID_HANDLE_VALUE) CloseHandle(log::g_file);
    DeleteFileW(g_path);
    std::printf("lk2_log_native_test: %d passed, %d failed; qpc-frequency=%llu\n",
                g_pass, g_fail, static_cast<unsigned long long>(g_freq));
    return g_fail ? 1 : 0;
}

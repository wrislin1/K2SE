#pragma once
#include <cstdint>

// ============================================================================
// living-kotor2 bridge: game-thread meters.
//
// Stage 3 asks for a number behind exit criterion E4 ("a script's LK2_ call
// never blocks"): the largest time the game thread ever spent inside LK2_Post
// or LK2_Poll, printed when the bridge is removed. MaxTimer keeps that maximum
// over counter values the caller supplies (QueryPerformanceCounter ticks in the
// DLL, plain integers in the tests) and TicksToMicros converts it.
//
// PollTraceThinner is the other meter: under the diagnostic marker every empty
// poll was one trace line (run 1 wrote 100 of them between "End." and quit).
// It passes the first report of each (id, latest) state and swallows identical
// repeats, telling the next written line how many it swallowed.
//
// No Windows headers; unit-tested on Linux (tests/lk2_meter_test.cpp).
// ============================================================================

namespace k2se {
namespace lk2 {

struct MaxTimer {
    uint64_t maxTicks = 0;
    uint32_t count = 0;
    uint32_t maxAt = 0;       // caller's tag for the first interval that set the maximum
    uint64_t overTicks = 0;   // threshold for `over`; 0 counts nothing
    uint32_t over = 0;        // intervals strictly longer than overTicks

    // Records one interval; an end before its start is a counter glitch and is
    // neither counted nor allowed to poison the maximum. `at` says when it
    // happened (the DLL passes the log's t= milliseconds).
    void Record(uint64_t start, uint64_t end, uint32_t at = 0) {
        if (end < start) return;
        const uint64_t d = end - start;
        if (d > maxTicks) {
            maxTicks = d;
            maxAt = at;
        }
        if (overTicks && d > overTicks) ++over;
        ++count;
    }
};

// `ticks` of a counter running at `freq` per second, in microseconds rounded to
// nearest; saturates at UINT32_MAX (about 71 minutes) and returns 0 for a zero
// frequency.
uint32_t TicksToMicros(uint64_t ticks, uint64_t freq);

// Raw QPC boundaries, deliberately not independently maximized or rounded.
struct TickSpan {
    uint64_t begin = 0, end = 0;
};

enum class WriteState : uint32_t { None, Skipped, Ok, Failed };

// Caller-owned: worker logging cannot overwrite a game-thread observation.
struct WriteSample {
    TickSpan span;
    WriteState state = WriteState::None;
    uint32_t requested = 0, written = 0, error = 0;
};

struct PollSample {
    TickSpan full, locked, log;
    WriteSample write;
    uint32_t at = 0, ordinal = 0;
    int id = 0, latest = 0;
    bool found = false, logCalled = false;
};

// Choose the pair with exactly the same endpoint and strict first-wins rule
// as the legacy full timer. A zero/backwards interval cannot set its maximum.
inline void RecordPoll(MaxTimer& timer, PollSample& worst, const PollSample& call) {
    if (call.full.end < call.full.begin) return;
    if (call.full.end - call.full.begin > timer.maxTicks) {
        worst = call;
        worst.ordinal = timer.count + 1;
    }
    timer.Record(call.full.begin, call.full.end, call.at);
}

class PollTraceThinner {
  public:
    PollTraceThinner() : haveLast_(false), lastId_(0), lastLatest_(0), suppressed_(0) {}

    // Call for every poll that found nothing. True means "write the line"; then
    // *suppressed is how many identical reports were swallowed since the last
    // written line. False means swallow this one.
    bool Note(int id, int latest, uint32_t* suppressed);

    // Swallowed reports not yet accounted for by a written line; cleared.
    uint32_t Flush();

  private:
    bool haveLast_;
    int lastId_;
    int lastLatest_;
    uint32_t suppressed_;
};

}  // namespace lk2
}  // namespace k2se

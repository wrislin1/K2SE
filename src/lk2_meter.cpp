#include "lk2_meter.h"

namespace k2se {
namespace lk2 {

uint32_t TicksToMicros(uint64_t ticks, uint64_t freq) {
    if (freq == 0) return 0;
    const uint64_t whole = ticks / freq;  // seconds
    const uint64_t rem = ticks % freq;    // < freq, so rem * 1e6 fits for any real QPC rate
    const uint64_t frac = (rem * 1000000ULL + freq / 2) / freq;
    if (whole >= 4295ULL) return 0xFFFFFFFFu;  // 4295 s already exceeds UINT32_MAX us
    const uint64_t us = whole * 1000000ULL + frac;
    return us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
}

bool PollTraceThinner::Note(int id, int latest, uint32_t* suppressed) {
    if (haveLast_ && id == lastId_ && latest == lastLatest_) {
        ++suppressed_;
        return false;
    }
    haveLast_ = true;
    lastId_ = id;
    lastLatest_ = latest;
    if (suppressed) *suppressed = suppressed_;
    suppressed_ = 0;
    return true;
}

uint32_t PollTraceThinner::Flush() {
    const uint32_t n = suppressed_;
    suppressed_ = 0;
    return n;
}

}  // namespace lk2
}  // namespace k2se

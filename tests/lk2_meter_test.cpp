// Unit tests for the bridge's game-thread meters: the max-duration tracker
// behind "bridge: game-thread max post=.. us poll=.. us" and the thinner that
// turns a run of identical empty-poll traces into one line per state change.
// Pure arithmetic over caller-supplied counter values, so it runs on Linux.
#include "lk2_meter.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (cond) { ++g_pass; } else {                                        \
            ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

using namespace k2se::lk2;

static void test_max_timer_keeps_the_largest_interval_and_counts_calls() {
    MaxTimer t;
    CHECK(t.maxTicks == 0); CHECK(t.count == 0);
    t.Record(100, 350);
    CHECK(t.maxTicks == 250); CHECK(t.count == 1);
    t.Record(1000, 1010);           // smaller: max unchanged, still counted
    CHECK(t.maxTicks == 250); CHECK(t.count == 2);
    t.Record(5000, 5900);
    CHECK(t.maxTicks == 900); CHECK(t.count == 3);
}

static void test_max_timer_ignores_a_backwards_interval() {
    MaxTimer t;
    t.Record(50, 40);
    CHECK(t.maxTicks == 0); CHECK(t.count == 0);
}

static void test_ticks_to_micros_rounds_to_nearest() {
    CHECK(TicksToMicros(250, 1000000) == 250);
    CHECK(TicksToMicros(10, 10000000) == 1);
    CHECK(TicksToMicros(15, 10000000) == 2);     // 1.5 us rounds up
    CHECK(TicksToMicros(4, 10000000) == 0);      // 0.4 us rounds down
    CHECK(TicksToMicros(0, 10000000) == 0);
    CHECK(TicksToMicros(3000000000ULL, 10000000) == 300000000u);  // 300 s, no overflow
    CHECK(TicksToMicros(100, 0) == 0);           // a zero frequency cannot divide
}

static void test_ticks_to_micros_saturates_at_uint32_max() {
    CHECK(TicksToMicros(50000000000000ULL, 10000000) == 4294967295u);  // 5e6 s
}

static void test_thinner_writes_the_first_report_of_each_state() {
    PollTraceThinner th;
    uint32_t s = 99;
    CHECK(th.Note(0, 0, &s)); CHECK(s == 0);
    CHECK(!th.Note(0, 0, &s));
    CHECK(!th.Note(0, 0, &s));
    CHECK(th.Note(0, 1, &s)); CHECK(s == 2);     // latest changed: two swallowed
    CHECK(th.Note(-1, 1, &s)); CHECK(s == 0);    // id changed, nothing swallowed
    CHECK(!th.Note(-1, 1, &s));
    CHECK(th.Note(0, 1, &s)); CHECK(s == 1);
}

static void test_thinner_flush_reports_and_clears_the_pending_count() {
    PollTraceThinner th;
    uint32_t s = 0;
    th.Note(0, 3, &s);
    th.Note(0, 3, &s); th.Note(0, 3, &s); th.Note(0, 3, &s);
    CHECK(th.Flush() == 3);
    CHECK(th.Flush() == 0);
    CHECK(!th.Note(0, 3, &s));                   // same state: still thinned after a flush
    CHECK(th.Note(0, 4, &s)); CHECK(s == 1);
}

int main() {
    test_max_timer_keeps_the_largest_interval_and_counts_calls();
    test_max_timer_ignores_a_backwards_interval();
    test_ticks_to_micros_rounds_to_nearest();
    test_ticks_to_micros_saturates_at_uint32_max();
    test_thinner_writes_the_first_report_of_each_state();
    test_thinner_flush_reports_and_clears_the_pending_count();
    std::printf("lk2_meter_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

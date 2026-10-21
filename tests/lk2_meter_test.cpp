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

static void test_max_timer_remembers_when_its_maximum_happened() {
    MaxTimer t;
    CHECK(t.maxAt == 0);
    t.Record(100, 350, 7);
    CHECK(t.maxAt == 7);
    t.Record(1000, 1010, 9);        // smaller: the time of the maximum stays
    CHECK(t.maxAt == 7);
    t.Record(5000, 5900, 12);
    CHECK(t.maxAt == 12);
    t.Record(6000, 6900, 15);       // equal: the first occurrence stays
    CHECK(t.maxAt == 12);
}

static void test_max_timer_counts_intervals_over_its_threshold() {
    MaxTimer t;
    t.Record(0, 5000, 1);           // no threshold set: nothing is counted over
    CHECK(t.over == 0);
    t.overTicks = 100;
    t.Record(0, 100, 2);            // at the threshold is not over it
    CHECK(t.over == 0);
    t.Record(0, 101, 3);
    CHECK(t.over == 1);
    t.Record(0, 50, 4);
    CHECK(t.over == 1);
    t.Record(300, 200, 5);          // backwards: not counted over either
    CHECK(t.over == 1);
    CHECK(t.count == 4);
}

// Catches replacing a paired record with independent lock/write maxima, or
// copying the most recent call even when it did not set the whole maximum.
static void test_poll_sample_belongs_to_the_whole_maximum() {
    MaxTimer t;
    t.overTicks = 1000;
    PollSample worst;
    PollSample a;
    a.full = {100, 1300}; a.locked = {110, 130}; a.log = {140, 1290};
    a.write.span = {250, 1250}; a.write.state = WriteState::Ok;
    a.write.requested = 65; a.write.written = 65;
    a.at = 77; a.id = -1; a.latest = 2; a.logCalled = true;
    RecordPoll(t, worst, a);
    PollSample b;
    b.full = {2000, 2700}; b.locked = {2001, 2501}; b.log = {2510, 2690};
    b.write.span = {2600, 2650}; b.write.state = WriteState::Ok;
    b.at = 88; b.id = 0; b.latest = 2; b.logCalled = true;
    RecordPoll(t, worst, b);
    PollSample c;
    c.full = {3000, 4100}; c.locked = {3001, 3006}; c.log = {3010, 4090};
    c.write.span = {3050, 4051}; c.write.state = WriteState::Failed;
    c.write.error = 5; c.at = 99; c.id = -1; c.latest = 3; c.logCalled = true;
    RecordPoll(t, worst, c);
    CHECK(t.count == 3); CHECK(t.over == 2); CHECK(t.maxTicks == 1200);
    CHECK(t.maxAt == 77); CHECK(worst.ordinal == 1); CHECK(worst.at == 77);
    CHECK(worst.id == -1); CHECK(worst.latest == 2); CHECK(!worst.found);
    CHECK(worst.full.begin == 100); CHECK(worst.full.end == 1300);
    CHECK(worst.locked.begin == 110); CHECK(worst.locked.end == 130);
    CHECK(worst.log.begin == 140); CHECK(worst.log.end == 1290);
    CHECK(worst.logCalled); CHECK(worst.write.state == WriteState::Ok);
    CHECK(worst.write.span.begin == 250); CHECK(worst.write.span.end == 1250);
    CHECK(worst.write.requested == 65); CHECK(worst.write.written == 65);
    CHECK(worst.write.error == 0);
}

static void test_poll_sample_replacement_ties_and_no_log() {
    MaxTimer t;
    PollSample worst, call;
    call.full = {10, 110}; call.at = 7; call.id = -1;
    call.logCalled = true; call.write.state = WriteState::Skipped;
    RecordPoll(t, worst, call);
    call.full = {200, 300}; call.at = 8;
    RecordPoll(t, worst, call);
    CHECK(worst.at == 7); CHECK(worst.ordinal == 1); CHECK(t.count == 2);
    call = PollSample{};
    call.full = {400, 601}; call.at = 9; call.id = 0; call.latest = 4;
    call.found = true;
    RecordPoll(t, worst, call);
    CHECK(worst.at == 9); CHECK(worst.ordinal == 3); CHECK(t.count == 3);
    CHECK(worst.id == 0); CHECK(worst.latest == 4); CHECK(worst.found);
    CHECK(!worst.logCalled); CHECK(worst.write.state == WriteState::None);
    CHECK(worst.log.begin == 0); CHECK(worst.log.end == 0);
    CHECK(worst.write.span.begin == 0); CHECK(worst.write.span.end == 0);
}

static void test_poll_sample_invalid_intervals_and_strict_bound() {
    MaxTimer t;
    t.overTicks = 1000;
    PollSample worst, call;
    call.full = {50, 40}; call.at = 1;
    RecordPoll(t, worst, call);
    CHECK(t.count == 0); CHECK(worst.ordinal == 0);
    call.full = {50, 50};
    RecordPoll(t, worst, call);
    CHECK(t.count == 1); CHECK(worst.ordinal == 0);
    call.full = {100, 1100}; call.at = 2;
    RecordPoll(t, worst, call);
    CHECK(t.count == 2); CHECK(t.over == 0); CHECK(worst.ordinal == 2);
    call.full = {2000, 3001}; call.at = 3;
    RecordPoll(t, worst, call);
    CHECK(t.count == 3); CHECK(t.over == 1); CHECK(t.maxTicks == 1001);
    call.full = {9999, 0}; call.at = 4;
    RecordPoll(t, worst, call);
    CHECK(t.count == 3); CHECK(t.over == 1); CHECK(worst.at == 3);
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
    test_max_timer_remembers_when_its_maximum_happened();
    test_max_timer_counts_intervals_over_its_threshold();
    test_poll_sample_belongs_to_the_whole_maximum();
    test_poll_sample_replacement_ties_and_no_log();
    test_poll_sample_invalid_intervals_and_strict_bound();
    test_ticks_to_micros_rounds_to_nearest();
    test_ticks_to_micros_saturates_at_uint32_max();
    test_thinner_writes_the_first_report_of_each_state();
    test_thinner_flush_reports_and_clears_the_pending_count();
    std::printf("lk2_meter_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

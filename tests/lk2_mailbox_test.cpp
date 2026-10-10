// Unit tests for the bridge mailbox: the two queues the game thread and the
// socket worker share. No locking inside the mailbox -- the caller locks -- so
// this is plain single-threaded logic and runs on Linux.
#include "lk2_mailbox.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (cond) { ++g_pass; } else {                                        \
            ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

using namespace k2se::lk2;

static int Post(Mailbox& m, const char* s) { return m.Post(s, static_cast<uint32_t>(std::strlen(s))); }
static Mailbox::Deliver Deliver(Mailbox& m, int id, const char* s, bool* cut = nullptr) {
    bool dummy = false;
    return m.DeliverReply(id, s, static_cast<uint32_t>(std::strlen(s)), cut ? cut : &dummy);
}
static bool Poll(Mailbox& m, int id, std::string* out) {
    char buf[kMaxText]; uint32_t len = 0;
    const bool ok = m.Poll(id, buf, sizeof(buf), &len);
    if (ok) out->assign(buf, len);
    return ok;
}
static std::string Take(Mailbox& m, bool* ok) {
    Message msg;
    *ok = m.TakeOutbound(&msg);
    return *ok ? std::string(msg.text, msg.len) : "";
}

static void test_post_assigns_increasing_ids_from_one() {
    Mailbox m;
    CHECK(m.LastPostedId() == 0);
    CHECK(Post(m, "a") == 1);
    CHECK(Post(m, "b") == 2);
    CHECK(m.LastPostedId() == 2);
}

static void test_take_outbound_is_fifo() {
    Mailbox m;
    Post(m, "first"); Post(m, "second");
    bool ok;
    CHECK(Take(m, &ok) == "first"); CHECK(ok);
    Message msg; CHECK(m.TakeOutbound(&msg)); CHECK(msg.id == 2); CHECK(std::string(msg.text, msg.len) == "second");
    CHECK(!m.TakeOutbound(&msg));
}

static void test_post_returns_zero_when_outbound_is_full_and_recovers() {
    Mailbox m;
    for (int i = 1; i <= kOutboundSlots; ++i) CHECK(Post(m, "x") == i);
    CHECK(Post(m, "overflow") == 0);
    CHECK(m.LastPostedId() == kOutboundSlots);
    Message msg; CHECK(m.TakeOutbound(&msg));
    CHECK(Post(m, "again") == kOutboundSlots + 1);
}

static void test_post_truncates_oversized_text() {
    Mailbox m;
    std::string big(kMaxText + 500, 'z');
    CHECK(m.Post(big.c_str(), static_cast<uint32_t>(big.size())) == 1);
    Message msg; CHECK(m.TakeOutbound(&msg));
    CHECK(msg.len == kMaxText - 1); CHECK(msg.text[msg.len] == '\0');
}

static void test_post_reports_truncation_to_the_caller() {
    Mailbox m;
    std::string big(kMaxText + 500, 'z');
    bool cut = false;
    CHECK(m.Post(big.c_str(), static_cast<uint32_t>(big.size()), &cut) == 1);
    CHECK(cut);
    cut = true;
    CHECK(m.Post("small", 5, &cut) == 2);
    CHECK(!cut);
}

static void test_reply_poll_by_exact_id_is_non_destructive() {
    Mailbox m;
    Post(m, "q");
    CHECK(Deliver(m, 1, "r1") == Mailbox::Deliver::kStored);
    std::string s;
    CHECK(Poll(m, 1, &s)); CHECK(s == "r1");
    CHECK(Poll(m, 1, &s)); CHECK(s == "r1");
}

static void test_poll_latest_follows_the_last_post() {
    Mailbox m;
    std::string s;
    CHECK(!Poll(m, kLatestId, &s));          // nothing posted yet
    Post(m, "q1"); Post(m, "q2");
    CHECK(!Poll(m, kLatestId, &s));          // posted, no reply yet
    CHECK(Deliver(m, 2, "two") == Mailbox::Deliver::kStored);
    CHECK(Poll(m, kLatestId, &s)); CHECK(s == "two");
    CHECK(!Poll(m, 1, &s));
}

static void test_control_messages_are_consumed_fifo() {
    Mailbox m;
    std::string s;
    CHECK(!Poll(m, kControlId, &s));
    CHECK(Deliver(m, 0, "start") == Mailbox::Deliver::kStored);
    CHECK(Deliver(m, 0, "second") == Mailbox::Deliver::kStored);
    CHECK(Poll(m, kControlId, &s)); CHECK(s == "start");
    CHECK(Poll(m, kControlId, &s)); CHECK(s == "second");
    CHECK(!Poll(m, kControlId, &s));
}

static void test_late_reply_is_dropped_on_arrival() {
    Mailbox m;
    Post(m, "q1"); Post(m, "q2");
    CHECK(Deliver(m, 1, "late") == Mailbox::Deliver::kLate);
    std::string s; CHECK(!Poll(m, 1, &s));
}

static void test_reply_for_an_id_never_posted_is_dropped() {
    Mailbox m;
    Post(m, "q1");
    CHECK(Deliver(m, 5, "bogus") == Mailbox::Deliver::kUnknown);
    std::string s; CHECK(!Poll(m, 5, &s));
}

static void test_new_post_evicts_older_replies_but_not_control() {
    Mailbox m;
    Post(m, "q1");
    Deliver(m, 1, "r1");
    Deliver(m, 0, "ctl");
    Post(m, "q2");
    std::string s;
    CHECK(!Poll(m, 1, &s));
    CHECK(Poll(m, kControlId, &s)); CHECK(s == "ctl");
    CHECK(m.InboundCount() == 0);
}

static void test_reply_text_is_truncated_to_capacity() {
    Mailbox m;
    Post(m, "q");
    std::string big(kMaxText + 10, 'y');
    bool cut = false;
    CHECK(m.DeliverReply(1, big.c_str(), static_cast<uint32_t>(big.size()), &cut) == Mailbox::Deliver::kStored);
    CHECK(cut);
    char buf[kMaxText]; uint32_t len = 0;
    CHECK(m.Poll(1, buf, sizeof(buf), &len)); CHECK(len == kMaxText - 1);
}

static void test_poll_into_a_small_buffer_cuts_and_terminates() {
    Mailbox m;
    Post(m, "q");
    Deliver(m, 1, "0123456789");
    char buf[4]; uint32_t len = 9;
    CHECK(m.Poll(1, buf, sizeof(buf), &len));
    CHECK(len == 3); CHECK(std::string(buf) == "012");
}

static void test_inbound_full_refuses_the_newest() {
    Mailbox m;
    for (int i = 0; i < kInboundSlots; ++i) CHECK(Deliver(m, 0, "c") == Mailbox::Deliver::kStored);
    CHECK(Deliver(m, 0, "one too many") == Mailbox::Deliver::kFull);
    CHECK(m.InboundCount() == kInboundSlots);
}

static void test_counts() {
    Mailbox m;
    CHECK(m.OutboundCount() == 0 && m.InboundCount() == 0);
    Post(m, "a"); Deliver(m, 1, "r"); Deliver(m, 0, "c");
    CHECK(m.OutboundCount() == 1); CHECK(m.InboundCount() == 2);
}

int main() {
    test_post_assigns_increasing_ids_from_one();
    test_take_outbound_is_fifo();
    test_post_returns_zero_when_outbound_is_full_and_recovers();
    test_post_truncates_oversized_text();
    test_post_reports_truncation_to_the_caller();
    test_reply_poll_by_exact_id_is_non_destructive();
    test_poll_latest_follows_the_last_post();
    test_control_messages_are_consumed_fifo();
    test_late_reply_is_dropped_on_arrival();
    test_reply_for_an_id_never_posted_is_dropped();
    test_new_post_evicts_older_replies_but_not_control();
    test_reply_text_is_truncated_to_capacity();
    test_poll_into_a_small_buffer_cuts_and_terminates();
    test_inbound_full_refuses_the_newest();
    test_counts();
    std::printf("lk2_mailbox_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

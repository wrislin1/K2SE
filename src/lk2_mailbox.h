#pragma once
#include <cstdint>

// ============================================================================
// living-kotor2 bridge: the mailbox the game thread and the socket worker share.
//
// Two fixed-size stores, no heap, no locking of its own (bridge.cpp wraps every
// call in one critical section, which keeps this file testable on Linux):
//
//   outbound  a FIFO of requests the game posted and the worker has yet to send
//   inbound   replies keyed by request id, plus control messages under id 0
//
// Ids: Post hands out 1, 2, 3 ... for the life of the process. kControlId (0)
// is the responder's unsolicited channel ("start"); kLatestId (-1) lets a
// script poll "whatever I posted last" without storing the id itself, because
// NWScript local numbers hold one byte and are not proven to persist.
//
// Staleness is decided by id, which is exit criterion E5: a reply arriving for
// an id older than the latest post is dropped on arrival (kLate), and a new
// post evicts every reply older than itself, so a late answer can never be
// shown as the answer to a newer question. Reply polls are non-destructive --
// the dialogue re-reads the same reply each time its node is shown -- while
// control messages are consumed, since "start" must fire once.
// ============================================================================

namespace k2se {
namespace lk2 {

constexpr int kControlId = 0;
constexpr int kLatestId = -1;
constexpr uint32_t kMaxText = 4096;  // bytes, including the terminator
constexpr int kOutboundSlots = 32;
constexpr int kInboundSlots = 32;

struct Message {
    int id;
    uint32_t len;         // bytes before the terminator
    char text[kMaxText];  // always NUL-terminated
};

class Mailbox {
  public:
    Mailbox();

    // Game thread. Queues `text` and returns its id (>= 1), or 0 if the outbound
    // queue is full -- nothing is queued and no id is consumed. Text longer
    // than kMaxText-1 is cut. Evicts every stored reply with an older id.
    int Post(const char* text, uint32_t len);

    // Worker. Oldest queued request, or false when there is none.
    bool TakeOutbound(Message* out);

    enum class Deliver { kStored, kLate, kUnknown, kFull };

    // Worker. Stores a reply for a posted id, or a control message under id 0.
    // kLate: id older than the latest post. kUnknown: id never posted (or
    // negative). kFull: no inbound slot free. `*truncated` reports a cut.
    Deliver DeliverReply(int id, const char* text, uint32_t len, bool* truncated);

    // Game thread. id > 0: that reply, left in place. kLatestId: the reply to
    // the most recent post. kControlId: the oldest control message, consumed.
    // Copies at most cap-1 bytes and terminates. False when nothing is there.
    bool Poll(int id, char* out, uint32_t cap, uint32_t* len);

    int LastPostedId() const { return lastId_; }
    int OutboundCount() const { return outCount_; }
    int InboundCount() const;

  private:
    struct Slot {
        bool used;
        uint32_t seq;  // arrival order, for FIFO control messages
        Message msg;
    };

    static void Fill(Message* m, int id, const char* text, uint32_t len, bool* truncated);
    int FindReply(int id) const;
    int FindOldestControl() const;
    int FindFree() const;

    Message out_[kOutboundSlots];
    int outHead_;
    int outCount_;
    Slot in_[kInboundSlots];
    uint32_t seq_;
    int lastId_;
    int nextId_;
};

}  // namespace lk2
}  // namespace k2se

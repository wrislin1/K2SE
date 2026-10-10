#include "lk2_mailbox.h"

#include <cstring>

namespace k2se {
namespace lk2 {

Mailbox::Mailbox() : outHead_(0), outCount_(0), seq_(0), lastId_(0), nextId_(1) {
    for (Slot& s : in_) s.used = false;
}

void Mailbox::Fill(Message* m, int id, const char* text, uint32_t len, bool* truncated) {
    const bool cut = len > kMaxText - 1;
    const uint32_t n = cut ? kMaxText - 1 : len;
    m->id = id;
    m->len = n;
    if (n) std::memcpy(m->text, text, n);
    m->text[n] = '\0';
    if (truncated) *truncated = cut;
}

int Mailbox::Post(const char* text, uint32_t len) {
    if (outCount_ >= kOutboundSlots) return 0;
    const int id = nextId_++;
    lastId_ = id;
    // Every stored reply answers an older question now.
    for (Slot& s : in_)
        if (s.used && s.msg.id > 0 && s.msg.id < id) s.used = false;
    Message& m = out_[(outHead_ + outCount_) % kOutboundSlots];
    Fill(&m, id, text, len, nullptr);
    ++outCount_;
    return id;
}

bool Mailbox::TakeOutbound(Message* out) {
    if (outCount_ == 0) return false;
    std::memcpy(out, &out_[outHead_], sizeof(Message));
    outHead_ = (outHead_ + 1) % kOutboundSlots;
    --outCount_;
    return true;
}

int Mailbox::FindReply(int id) const {
    for (int i = 0; i < kInboundSlots; ++i)
        if (in_[i].used && in_[i].msg.id == id) return i;
    return -1;
}

int Mailbox::FindOldestControl() const {
    int best = -1;
    for (int i = 0; i < kInboundSlots; ++i) {
        if (!in_[i].used || in_[i].msg.id != kControlId) continue;
        if (best < 0 || in_[i].seq < in_[best].seq) best = i;
    }
    return best;
}

int Mailbox::FindFree() const {
    for (int i = 0; i < kInboundSlots; ++i)
        if (!in_[i].used) return i;
    return -1;
}

Mailbox::Deliver Mailbox::DeliverReply(int id, const char* text, uint32_t len, bool* truncated) {
    if (truncated) *truncated = false;
    int slot = -1;
    if (id == kControlId) {
        slot = FindFree();
    } else {
        if (id < 0 || id > lastId_) return Deliver::kUnknown;
        if (id < lastId_) return Deliver::kLate;
        slot = FindReply(id);  // a duplicate reply overwrites the first
        if (slot < 0) slot = FindFree();
    }
    if (slot < 0) return Deliver::kFull;
    Slot& s = in_[slot];
    s.used = true;
    s.seq = ++seq_;
    Fill(&s.msg, id, text, len, truncated);
    return Deliver::kStored;
}

bool Mailbox::Poll(int id, char* out, uint32_t cap, uint32_t* len) {
    int slot = -1;
    bool consume = false;
    if (id == kLatestId) {
        if (lastId_ == 0) return false;
        slot = FindReply(lastId_);
    } else if (id == kControlId) {
        slot = FindOldestControl();
        consume = true;
    } else if (id > 0) {
        slot = FindReply(id);
    }
    if (slot < 0) return false;
    const Message& m = in_[slot].msg;
    uint32_t n = m.len;
    if (cap == 0) {
        n = 0;
    } else if (n > cap - 1) {
        n = cap - 1;
    }
    if (n) std::memcpy(out, m.text, n);
    if (cap) out[n] = '\0';
    if (len) *len = n;
    if (consume) in_[slot].used = false;
    return true;
}

int Mailbox::InboundCount() const {
    int n = 0;
    for (const Slot& s : in_) n += s.used ? 1 : 0;
    return n;
}

}  // namespace lk2
}  // namespace k2se

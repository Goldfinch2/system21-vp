// c139.cpp: the C139 (c139.h), from what the slave's program expects of it
// (docs/link.md):
//
//   status (reg 0): bit 1 a packet received, bit 2 ready to send (idle);
//     writing clears "received";
//   interrupt mask (reg 1): bit 1 the receive interrupt, bit 2 the send
//     one, set = masked; the chip sets a bit as it interrupts for it, the
//     game clears it to re-arm (its handler's ANDI #9, #B, #D);
//   reg 3: 1 while the game starts a send; reg 5: the length, writing it
//     sends; reg 6: the receive ring's word last written; reg 7: the packet's
//     first word.
#include "c139.h"

#include "link.h"
#include "state.h"

namespace {

const uint16_t RX = 0x2, TX = 0x4;
const int RING = 0x1000, RING_WORDS = 0x1000;   // the receive ring: RAM words 1000h-1FFFh
const long SEND_CYCLES = 8000;                  // a 94-word packet on the wire: under a millisecond

} // namespace

void C139::attach(uint16_t *ram, Link *link)
{
    ram_ = ram;
    link_ = link;
}

void C139::reset()
{
    for (uint16_t &r : regs_) r = 0;
    received_ = false;
    sending_ = 0;
    ringPos_ = 0;
    lastWord_ = RING + RING_WORDS - 1;
    interrupt_ = false;
    waiting_.clear();
}

bool C139::inUse() const
{
    return link_ && link_->isOpen();
}

uint16_t C139::read(int reg) const
{
    if (!inUse()) return reg == 0 ? 4 : 0;            // as before the link: idle, nothing more
    switch (reg) {
    case 0: return (uint16_t)((received_ ? RX : 0) | (sending_ ? 0 : TX));
    case 6: return (uint16_t)lastWord_;
    default: return regs_[reg & 7];
    }
}

void C139::write(int reg, uint16_t data, uint16_t mask)
{
    if (!inUse()) return;
    reg &= 7;
    regs_[reg] = (uint16_t)((regs_[reg] & ~mask) | (data & mask));
    if (reg == 0) {
        received_ = false;                            // the packet was taken: the next one may come
        deliver();
    } else if (reg == 5 && regs_[3] && ram_) {
        // A send: the packet from its first word (reg 7), reg 5 words long.
        const int len = regs_[5] & 0x1ff, start = regs_[7] & 0x1fff;
        std::vector<uint16_t> words((size_t)len);
        for (int i = 0; i < len; i++) words[(size_t)i] = ram_[(start + i) & 0x1fff];
        if (link_) link_->send(words.data(), len);
        sending_ = SEND_CYCLES;
    }
    check();
}

void C139::advance(int cycles)
{
    if (!sending_) return;
    sending_ -= cycles;
    if (sending_ <= 0) {
        sending_ = 0;
        check();
    }
}

void C139::poll()
{
    if (!inUse()) {
        waiting_.clear();
        return;
    }
    std::vector<uint16_t> words;
    while (link_->receive(words)) waiting_.push_back(std::move(words));
    while (waiting_.size() > 16) waiting_.pop_front();   // fell behind: the old ones are stale
    deliver();
}

// The next packet into the receive ring, once the game has taken the last.
void C139::deliver()
{
    if (received_ || waiting_.empty() || !ram_) return;
    const std::vector<uint16_t> &w = waiting_.front();
    for (uint16_t v : w) {
        lastWord_ = RING + ringPos_;
        ram_[lastWord_] = v;
        ringPos_ = (ringPos_ + 1) % RING_WORDS;
    }
    waiting_.pop_front();
    received_ = true;
    check();
}

// An interrupt for each cause not masked; it masks itself as it goes.
void C139::check()
{
    const uint16_t status = read(0), due = (uint16_t)(status & ~regs_[1] & (RX | TX));
    if (due) {
        regs_[1] |= due;
        interrupt_ = true;
    }
}

bool C139::takeInterrupt()
{
    const bool due = interrupt_;
    interrupt_ = false;
    return due;
}

void C139::state(State &s)
{
    s.bytes(regs_, sizeof regs_);
    s.pod(received_);
    s.pod(sending_);
    s.pod(ringPos_);
    s.pod(lastWord_);
}

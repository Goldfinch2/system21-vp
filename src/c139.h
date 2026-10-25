// c139.h: the C139 serial chip, as Cyber Sled's slave 68000 uses it for the
// link (docs/link.md): it sends a packet of its RAM to the next cabinet,
// writes the packets that arrive into its RAM's receive ring, and
// interrupts. Over the network (link.h). Not in use (no link), it answers as
// before: its status reads 4, and it never interrupts.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

class Link;
class State;

class C139 {
public:
    // ram: its 8K words (the machine's); link: the network, or null.
    void attach(uint16_t *ram, Link *link);
    void reset();
    bool inUse() const;

    // Its registers (B80000h + 2 x reg).
    uint16_t read(int reg) const;
    void write(int reg, uint16_t data, uint16_t mask);

    // Time passing: a packet being sent goes out after some.
    void advance(int cycles);
    // Packets from the other cabinet into the receive ring (what the link
    // has); pollNetwork reads the network first. Several times a frame: the
    // arcade's boards heard each other at once.
    void poll();
    void pollNetwork();
    // Its interrupt is due (once: then masked until the game re-arms it).
    bool takeInterrupt();

    void state(State &s);

private:
    void deliver();
    void check();

    uint16_t *ram_ = nullptr;
    Link *link_ = nullptr;
    uint16_t regs_[8] = {};
    bool received_ = false;         // status bit 1: a packet is in the ring
    long sending_ = 0;              // cycles until the packet being sent is out (0: idle)
    int ringPos_ = 0;               // the next word of the receive ring to write (0-FFFh)
    int lastWord_ = 0x1fff;         // B8000Ch: the word last written
    bool interrupt_ = false;
    std::deque<std::vector<uint16_t>> waiting_;
};

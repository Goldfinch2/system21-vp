// hle.h: the chips a System 21 game's 68000s talk to, replaced by what they do.
// Each sees the shared memory the way the real chip did and answers the same.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include <vectorpie/vp_input.h>

class State;

// ---- the C68 I/O MCU -----------------------------------------------------
// In the dual-port RAM (0x000-0x0ff, byte offsets as the 68000 sees them / 2):
//   00 buttons held: 80 start, 40 button 3, 20 button 2 (gun), 10 button 1 (gun 2, the same trigger)
//   03 buttons pressed this frame (same bits)
//   02 coin switches held: 10 coin 1, 08 coin 2;  05 coins inserted this frame
//   08-0F analog channels (the profile's sticks, center 80; others FF;
//         Star Blade 09 X, 0A Y: as its NVRAM calibrates them)
//   19 coins counted on coin 1 (1B on coin 2), shown as CREDIT n/2
//   16 written by the game: coins to take (a start, a continue); we subtract it
//   3A/3B/3C boot handshake; 1F/21/23 frame counters
class Mcu {
public:
    void setInput(const vp_input *in) { in_ = in; }
    // The game's NVRAM (one byte an address): its own stick calibration.
    void setNvram(const std::vector<uint8_t> *nv) { nv_ = nv; }
    // The aim speed (1 normal): the stick's range, the aim keys' and the mouse's pace.
    void setAimSpeed(float k) { aimSpeed_ = k; }
    void reset(uint8_t *dp);
    void vblank(uint8_t *dp);
    void write(uint8_t *dp, int offset, uint8_t data);
    void state(State &s);           // save or load what it keeps between frames

private:
    const vp_input *in_ = nullptr;
    const std::vector<uint8_t> *nv_ = nullptr;
    uint8_t prevButtons_ = 0;
    uint8_t coinPending_ = 0;     // coin bits to report as inserted next frame
    int coins_[2] = {};           // coins counted on coin 1, coin 2
    uint8_t meter_[2] = {};       // coin meters
    static const int MAX_STICKS = 2;
    float keyX_[MAX_STICKS] = {}, keyY_[MAX_STICKS] = {};   // a stick as its keys push it (-1..1)
    float mouseX_ = 0, mouseY_ = 0;   // and as the mouse / trackball moves it
    // How far each stick's axes have gone each way (X-, X+, Y-, Y+): that is
    // all the way over (yokes of one model report different travels).
    float reach_[MAX_STICKS][4] = { { REACH_START, REACH_START, REACH_START, REACH_START },
                                    { REACH_START, REACH_START, REACH_START, REACH_START } };
    static constexpr float REACH_START = 0.5f;
    float aimSpeed_ = 1.0f;
};

// ---- the DSP board ---------------------------------------------------------------
// Word offsets in the shared DSP RAM (68000 0x200000 + 2 x offset):
//   0x089/0x088 boot requests (0x200112 / 0x200110 ...), 0x101 status
//   0x103 page select: the page the 68000 fills next; the other is finished
//   0x4000 + page x 0x2000: camera block, object list at +0x100
class Dsp {
public:
    struct Frame {
        bool valid = false;
        uint16_t camera[0x40] = {};
        std::vector<uint16_t> list;     // the object list area (records, then FFFFh)
    };

    void reset();
    void write(uint16_t *ram, int word);
    void vblank(uint16_t *ram);
    const Frame &finished() const { return frame_; }
    // The game has started the DSPs (the end of its boot self test), and how
    // many times since power-on (a reset of the DSP board does not count back).
    bool running() const { return running_; }
    int starts() const { return starts_; }
    void clearStarts() { starts_ = 0; }     // a power-on: the boot counts again
    void state(State &s);

private:
    Frame frame_;
    bool running_ = false;
    int starts_ = 0;                // since power-on: not cleared by reset()
};

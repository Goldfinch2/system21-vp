// hle.cpp: the I/O MCU and the DSP board, as the 68000s see them. The values
// come from captures of the original running in MAME. The sound board runs
// its own program: soundboard.cpp.
#include "hle.h"
#include "profile.h"
#include "state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>


// ---- the C68 I/O MCU --------------------------------------------------------------

void Mcu::reset(uint8_t *dp)
{
    // What the C68 writes when it comes out of reset.
    memset(dp, 0, 0x60);
    dp[0x06] = 0x01;
    dp[0x07] = 0x09;
    for (int i = 0x08; i <= 0x0f; i++) dp[i] = 0xff;
    const Profile &game = profile();
    for (size_t k = 0; k < game.nsticks; k++)         // the sticks, centered
        dp[game.sticks[k].channelX] = dp[game.sticks[k].channelY] = 0x80;
    dp[0x11] = 0x01;
    dp[0x12] = 0x01;
    dp[0x13] = 0x20;
    dp[0x14] = 0x02;
    dp[0x1d] = 0x07;
    dp[0x1f] = 0x01;
    dp[0x3a] = 0xa6;                // boot handshake: "MCU here"
    dp[0x44] = 0x01;
    dp[0x56] = 0x01;
    dp[0x57] = 0x01;
    prevButtons_ = 0;
    coinPending_ = 0;
}

// A stick's position (-1..1) as its channel reads it: centered 80h, all the
// way over at lo or hi (the profile's).
static uint8_t stick(float v, int lo, int hi)
{
    v = std::max(-1.0f, std::min(1.0f, v));
    const int x = 0x80 + (int)(v * (v < 0 ? 0x80 - lo : hi - 0x80) + (v < 0 ? -0.5f : 0.5f));
    return (uint8_t)std::max(lo, std::min(hi, x));
}

void Mcu::vblank(uint8_t *dp)
{
    const Profile &game = profile();
    uint8_t buttons = 0, coinsHeld = 0, coinsIn = 0;
    float x[MAX_STICKS] = {}, y[MAX_STICKS] = {};
    if (in_) {
        if (vp_input_held(in_, "start1")) buttons |= 0x80;
        // the game's buttons (profile.h: Star Blade's gun 20h, gun 2 10h: the same trigger)
        for (size_t k = 0; k < game.nfunctions; k++)
            if (game.functions[k].bit && vp_input_held(in_, game.functions[k].name)) buttons |= game.functions[k].bit;
        if (vp_input_held(in_, "coin1")) coinsHeld |= 0x10;
        if (vp_input_held(in_, "coin2")) coinsHeld |= 0x08;
        if (vp_input_pressed(in_, "coin1")) coinsIn |= 0x10;
        if (vp_input_pressed(in_, "coin2")) coinsIn |= 0x08;
        // Each stick: its axes, then the keys that push it (aim_up ...:
        // profile.cpp) like a stick that springs back to the center when
        // released (as MAME does for analog controls), and the mouse or
        // trackball. While the stick is out of its center its keys are left
        // out: a stick bound both ways (its axis, and its directions to the
        // keys, AdvanceMAME's default) would have them fight.
        // Star Blade's Y: the low value aims DOWN (a flight stick: pull back
        // to aim up). Its stick is turned (upHigh), so up aims up on
        // everything: the axis (up is its negative side), the keys, the mouse.
        auto toward = [this](float v, float target) {
            const float rate = 0.06f * aimSpeed_;
            return v < target ? std::min(v + rate, target) : std::max(v - rate, target);
        };
        auto held = [this](const char *function) { return function && vp_input_held(in_, function) ? 1.0f : 0.0f; };
        const float CENTERED = 0.1f;
        for (size_t k = 0; k < game.nsticks && k < MAX_STICKS; k++) {
            const Stick &st = game.sticks[k];
            x[k] = vp_input_axis(in_, st.axisX);
            y[k] = -vp_input_axis(in_, st.axisY);      // up (the axis's negative side) is +
            // Calibrated as it goes: the furthest each way so far is all the
            // way over (one yoke reports half its axis, another all of it).
            auto calibrated = [](float v, float *reach) {
                float &r = reach[v < 0 ? 0 : 1];
                r = std::max(r, std::fabs(v));
                return v / r;
            };
            x[k] = calibrated(x[k], reach_[k]);
            y[k] = calibrated(y[k], reach_[k] + 2);
            float kx = held(st.right) - held(st.left);
            float ky = held(st.up) - held(st.down);
            if (std::fabs(x[k]) > CENTERED) kx = 0;
            if (std::fabs(y[k]) > CENTERED) ky = 0;
            keyX_[k] = toward(keyX_[k], kx);
            keyY_[k] = toward(keyY_[k], ky);
            // The aim speed: the stick reaches the edge with less travel (FAST) or more.
            x[k] = x[k] * aimSpeed_ + keyX_[k];
            y[k] = y[k] * aimSpeed_ + keyY_[k];
            if (st.mouse) {
                const float mouse = aimSpeed_ / 250.0f;
                mouseX_ = std::max(-1.0f, std::min(1.0f, mouseX_ + vp_input_delta(in_, "p1_trackballx") * mouse));
                mouseY_ = std::max(-1.0f, std::min(1.0f, mouseY_ - vp_input_delta(in_, "p1_trackbally") * mouse));
                x[k] += mouseX_;
                y[k] += mouseY_;
            }
            x[k] = std::max(-1.0f, std::min(1.0f, x[k]));
            y[k] = std::max(-1.0f, std::min(1.0f, y[k]));
            if (!st.upHigh) y[k] = -y[k];
        }
    }

    dp[0x00] = buttons;
    dp[0x03] = buttons & ~prevButtons_;
    prevButtons_ = buttons;

    // A coin: the switch (02), then the next frame "inserted" (05) and the count.
    dp[0x02] = coinsHeld;
    dp[0x05] = coinPending_;
    if (coinPending_ & 0x10) { if (coins_[0] < 99) coins_[0]++; meter_[0]++; }
    if (coinPending_ & 0x08) { if (coins_[1] < 99) coins_[1]++; meter_[1]++; }
    coinPending_ = coinsIn;

    // The game takes coins for a start or a continue by writing how many to 16/17.
    int take = dp[0x16] | dp[0x17] << 8;
    if (take) {
        int from1 = take < coins_[0] ? take : coins_[0];
        coins_[0] -= from1;
        take -= from1;
        coins_[1] -= take < coins_[1] ? take : coins_[1];
        dp[0x16] = dp[0x17] = 0;
    }
    dp[0x18] = 0;
    dp[0x19] = (uint8_t)coins_[0];
    dp[0x1a] = 0;
    dp[0x1b] = (uint8_t)coins_[1];
    dp[0x46] = meter_[0];
    dp[0x47] = meter_[1];

    for (size_t k = 0; k < game.nsticks && k < MAX_STICKS; k++) {
        const Stick &st = game.sticks[k];
        dp[st.channelX] = stick(x[k], st.loX, st.hiX);
        dp[st.channelY] = stick(y[k], st.loY, st.hiY);
    }

    dp[0x1f]++;
    dp[0x21]++;
    dp[0x23]++;
    dp[0x06] = 0x01;
    dp[0x44] = 0x01;
    if (dp[0x35] == 0xff) dp[0x35] = 0x00;
    if (dp[0x3b] == 0xff) { dp[0x3c] = 0xa6; dp[0x3b] = 0x00; }   // boot handshake
}

void Mcu::state(State &s)
{
    s.pod(prevButtons_);
    s.pod(coinPending_);
    s.bytes(coins_, sizeof coins_);
    s.bytes(meter_, sizeof meter_);
    s.bytes(keyX_, sizeof keyX_); s.bytes(keyY_, sizeof keyY_);
    s.pod(mouseX_); s.pod(mouseY_);
}

void Mcu::write(uint8_t *, int, uint8_t)
{
    // Everything the game asks of the MCU is answered at the next vblank.
}

// ---- the DSP board ---------------------------------------------------------------------

void Dsp::state(State &s)
{
    s.pod(frame_.valid);
    s.bytes(frame_.camera, sizeof frame_.camera);
    s.vec(frame_.list, true);
    s.pod(running_);
}

void Dsp::reset()
{
    frame_ = Frame();
    running_ = false;
}

void Dsp::write(uint16_t *ram, int word)
{
    if (word != 0x103) return;
    // The value written is the page the 68000 fills NEXT (it often starts
    // at once); the other one is finished: keep a copy for the renderer, as
    // the DSPs would start drawing it now.
    const uint16_t *page = ram + 0x4000 + ((ram[0x103] & 1) ^ 1) * 0x2000;
    memcpy(frame_.camera, page, sizeof frame_.camera);
    // The whole list area: FFFFh ends the list only where a record would
    // start (it is also -1 inside records); the renderer walks the records.
    frame_.list.assign(page + 0x100, page + 0x2000);
    frame_.valid = true;
}

void Dsp::vblank(uint16_t *ram)
{
    // The boot handshake (docs/dsp.md): what the master DSP answers.
    if (ram[0x89] != 0) {               // 0x200112: are you there?
        ram[0x89] = 0;
        // Star Blade's DSP answers with its point ROM checksums (0x20010A/C)
        // and shows the first in hex at 0x200024 ("EE52", the PTR: line).
        // Cyber Sled's reports none: the game writes its own text there.
        const uint16_t sum = profile().pointSums[0];
        if (sum || profile().pointSums[1]) {
            ram[0x85] = sum;
            ram[0x86] = profile().pointSums[1];
            auto hex = [](int v) { return (uint16_t)(v < 10 ? '0' + v : 'A' + v - 10); };
            ram[0x12] = hex(sum >> 12) << 8 | hex(sum >> 8 & 15);
            ram[0x13] = hex(sum >> 4 & 15) << 8 | hex(sum & 15);
        }
        ram[0x88] = 1;                  // 0x200110: ready
    }
    if (ram[0x87] != 0) ram[0x87] = 0;              // 0x20010E: program upload done
    if (ram[0x101] == 0xffff) {                     // 0x200202: running
        ram[0x101] = 0;
        running_ = true;
    }
}

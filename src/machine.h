// machine.h: Namco System 21 (C67 DSP board) as its games use it: the two
// 68000s run the game's own code (Musashi); everything around them is ours.
//
//   - the DSP board (5 TMS320C25) is replaced by Dsp: it answers the boot
//     handshake and hands each finished frame (camera + object list) to the
//     3D renderer, which draws it as vectors;
//   - the C68 I/O MCU is replaced by Mcu: controls and coins from the SDK,
//     written into the dual-port RAM where the game reads them;
//   - the sound board (6809, C140, YM2151) runs its own program in Sound
//     (soundboard.cpp): it reads the game's sound commands from the
//     dual-port RAM as on the real board;
//   - the C355 sprites (HUD, text, explosions) are drawn as vectors from
//     sprite RAM (sprites.cpp).
// What each one does is in ../docs/dsp.md and ../docs/hardware.md.
#pragma once

#include <cstdint>
#include <vector>

#include "c139.h"
#include "roms.h"

class Link;
class Mcu;
class Sound;
class Dsp;
class State;

class Machine {
public:
    static constexpr int kCpuClock = 12288000;              // 49.152 MHz / 4
    static constexpr double kFrameRate = 38769220.0 / 4 * 2 / 616 / 525;   // 59.94 Hz
    static constexpr int kVisibleLines = 480, kTotalLines = 525;

    Machine(Roms &roms, Mcu &mcu, Sound &sound, Dsp &dsp);
    ~Machine();

    void reset();
    // One video frame: the visible part, then vertical blank.
    void runFrame();
    long frame() const { return frame_; }      // frames since reset, as MAME's frame_number

    // ---- what the renderers read (between frames) ----
    const Roms &roms() const { return roms_; }
    const uint16_t *dspram() const { return dspram_; }
    uint16_t *dspram() { return dspram_; }
    uint8_t *dpram() { return dpram_; }
    const uint16_t *spriteram() const { return spriteram_.data(); }
    const uint16_t *spritePosition() const { return spritePos_; }
    uint32_t paletteRgb(int index) const;   // 0xRRGGBB
    bool videoEnabled() const { return videoEnable_ & 0x40; }

    std::vector<uint8_t> &nvram() { return nvram_; }
    // The link to another cabinet (the C139: docs/link.md), or null.
    void setLink(Link *link) { c139_.attach(c139Ram_.data(), link); }

    // Save or load the whole machine (its CPUs, memory, C148s, the I/O MCU,
    // the DSP board and the sound board), between frames.
    void state(State &s);

    // ---- memory, for the Musashi callbacks (cpu 0 = master, 1 = slave) ----
    uint16_t read16(int cpu, uint32_t a);
    void write16(int cpu, uint32_t a, uint16_t data, uint16_t mask);
    int intAck(int cpu, int level);

private:
    struct Cpu {
        std::vector<uint8_t> context;   // Musashi's state while another CPU runs
        bool inReset = false;
        bool needsReset = false;        // reset when it next runs
        long long debt = 0;             // cycles owed (+) or run ahead (-)
        uint8_t line[8] = {};           // per level: 0 clear, 1 asserted, 2 held until acknowledged
    };
    struct C148 {
        uint8_t bus = 0, cpuLevel = 0, exLevel = 0, posLevel = 0, sciLevel = 0, vblankLevel = 0;
    };

    void runCycles(int cycles);
    void select(int cpu);
    void setLine(int cpu, int level, int state);
    void applyIrq(int cpu);
    void resetC148(int cpu);
    void subcpuReset(bool assert);
    uint16_t c148Read(int cpu, uint32_t offset);
    void c148Write(int cpu, uint32_t offset, uint16_t data);
    void spriteramWrite(uint32_t offset, uint16_t data, uint16_t mask);
    void c139Interrupt();

    Roms &roms_;
    Mcu &mcu_;
    Sound &sound_;
    Dsp &dsp_;

    Cpu cpu_[2];
    C148 c148_[2];
    int current_ = -1;
    long frame_ = 0;
    bool subcpusHeld_ = true;

    std::vector<uint16_t> mainRam_, slaveRam_, sharedRam_, spriteram_, palette_, paletteExt_, c139Ram_;
    std::vector<uint8_t> nvram_;
    uint16_t dspram_[0x8000] = {};
    uint8_t dpram_[0x800] = {};
    uint16_t spritePos_[4] = {};
    uint16_t videoEnable_ = 0;
    std::vector<uint8_t> pointram_;
    int pointramIdx_ = 0;
    uint16_t pointramControl_ = 0;
    uint8_t depthcue_[2][0x400] = {};
    C139 c139_;
};

// machine.cpp: the two 68000s, their memory maps (MAME's namcos21_c67.cpp
// common_map / master_map / slave_map) and the C148 interrupt controllers.
#include "machine.h"

#include <cstring>

#include "hle.h"
#include "soundboard.h"
#include "state.h"

extern "C" {
#include "m68k.h"
}

namespace {

Machine *g_machine = nullptr;
int g_cpu = 0;

// Lines held (HOLD_LINE) are cleared when the CPU takes the interrupt.
enum { kClear = 0, kAssert = 1, kHold = 2 };

constexpr int kSliceCycles = 256;     // CPUs run in turn ~48000 times a second

int intAckCallback(int level) { return g_machine->intAck(g_cpu, level); }

} // namespace

// ---- Musashi's memory callbacks ---------------------------------------------------

extern "C" {

unsigned int m68k_read_memory_8(unsigned int a)
{
    uint16_t w = g_machine->read16(g_cpu, a & 0xfffffe);
    return (a & 1) ? (w & 0xff) : (w >> 8);
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    return g_machine->read16(g_cpu, a & 0xfffffe);
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    a &= 0xfffffe;
    uint32_t hi = g_machine->read16(g_cpu, a);
    return hi << 16 | g_machine->read16(g_cpu, (a + 2) & 0xffffff);
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    if (a & 1) g_machine->write16(g_cpu, a & 0xfffffe, v & 0xff, 0x00ff);
    else       g_machine->write16(g_cpu, a & 0xfffffe, (v & 0xff) << 8, 0xff00);
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    g_machine->write16(g_cpu, a & 0xfffffe, v & 0xffff, 0xffff);
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    a &= 0xfffffe;
    g_machine->write16(g_cpu, a, v >> 16, 0xffff);
    g_machine->write16(g_cpu, (a + 2) & 0xffffff, v & 0xffff, 0xffff);
}

// For m68kdasm.c (the disassembler, for debugging).
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

} // extern "C"

// ---- the machine ---------------------------------------------------------------------

Machine::Machine(Roms &roms, Mcu &mcu, Sound &sound, Dsp &dsp)
    : roms_(roms), mcu_(mcu), sound_(sound), dsp_(dsp)
{
    g_machine = this;
    mainRam_.assign(0x10000 / 2, 0);
    slaveRam_.assign(0x40000 / 2, 0);
    sharedRam_.assign(0x10000 / 2, 0);
    spriteram_.assign(0x20000 / 2, 0);
    palette_.assign(0x10000 / 2, 0);
    paletteExt_.assign(0x10000 / 2, 0);
    c139Ram_.assign(0x4000 / 2, 0);
    c139_.attach(c139Ram_.data(), nullptr);
    pointram_.assign(0x20000, 0);
    nvram_ = roms.nvram;
    mcu_.setNvram(&nvram_);

    m68k_init();
    for (int i = 0; i < 2; i++) {
        g_cpu = i;
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        m68k_set_int_ack_callback(intAckCallback);
        m68k_pulse_reset();
        cpu_[i].context.resize(m68k_context_size());
        m68k_get_context(cpu_[i].context.data());
    }
    current_ = -1;
}

Machine::~Machine()
{
    if (g_machine == this) g_machine = nullptr;
}

void Machine::reset()
{
    for (int i = 0; i < 2; i++) {
        resetC148(i);
        memset(cpu_[i].line, 0, sizeof cpu_[i].line);
        cpu_[i].debt = 0;
    }
    frame_ = 0;
    c139_.reset();
    select(0);
    m68k_pulse_reset();
    applyIrq(0);
    // MAME's machine_reset: the sound CPU, the slave and the I/O MCU held in reset.
    sound_.reset(true);
    subcpusHeld_ = false;
    subcpuReset(true);
}

void Machine::state(State &s)
{
    // The running CPU's registers are Musashi's; the other's are in its slot.
    if (s.saving() && current_ >= 0) m68k_get_context(cpu_[current_].context.data());
    for (Cpu &c : cpu_) {
        s.vec(c.context);
        s.pod(c.inReset);
        s.pod(c.needsReset);
        s.pod(c.debt);
        s.bytes(c.line, sizeof c.line);
    }
    s.bytes(c148_, sizeof c148_);
    s.pod(frame_);
    s.pod(subcpusHeld_);
    s.vec(mainRam_); s.vec(slaveRam_); s.vec(sharedRam_); s.vec(spriteram_);
    s.vec(palette_); s.vec(paletteExt_); s.vec(c139Ram_);
    c139_.state(s);
    s.vec(nvram_);
    s.bytes(dspram_, sizeof dspram_);
    s.bytes(dpram_, sizeof dpram_);
    s.bytes(spritePos_, sizeof spritePos_);
    s.pod(videoEnable_);
    s.vec(pointram_);
    s.pod(pointramIdx_);
    s.pod(pointramControl_);
    s.bytes(depthcue_, sizeof depthcue_);
    mcu_.state(s);
    dsp_.state(s);
    sound_.state(s);
    if (!s.saving() && s.ok()) {
        // A Musashi context also holds pointers (its cycle tables, its
        // callbacks), valid only in the run that saved it: Windows loads the
        // program at another address each time. This run's, in each slot.
        for (int i = 0; i < 2; i++) {
            m68k_set_context(cpu_[i].context.data());
            m68k_init();                // the callbacks: the defaults
            m68k_set_cpu_type(M68K_CPU_TYPE_68000);     // the tables (registers untouched)
            m68k_set_int_ack_callback(intAckCallback);
            m68k_get_context(cpu_[i].context.data());
        }
        current_ = -1;                  // the loaded slots are the truth: none saved over
        select(0);
    }
}

void Machine::select(int cpu)
{
    if (current_ == cpu) return;
    if (current_ >= 0) m68k_get_context(cpu_[current_].context.data());
    m68k_set_context(cpu_[cpu].context.data());
    current_ = cpu;
    g_cpu = cpu;
}

void Machine::runFrame()
{
    const long long frame = (long long)(kCpuClock / kFrameRate + 0.5);
    const long long visible = frame * kVisibleLines / kTotalLines;
    runCycles((int)visible);

    // Vertical blank: both C148s interrupt their CPU, the MCU reads the
    // controls, the DSP board does its once-a-frame work.
    for (int i = 0; i < 2; i++) {
        if (c148_[i].vblankLevel) setLine(i, c148_[i].vblankLevel, kHold);
    }
    if (!subcpusHeld_) mcu_.vblank(dpram_);
    dsp_.vblank(dspram_);
    c139_.poll();                       // the other cabinet's packets
    c139Interrupt();

    runCycles((int)(frame - visible));
    frame_++;
    sound_.runFrame();                  // the sound board, for the same frame
}

void Machine::runCycles(int cycles)
{
    for (int i = 0; i < 2; i++) cpu_[i].debt += cycles;
    while (cpu_[0].debt > 0 || cpu_[1].debt > 0) {
        for (int i = 0; i < 2; i++) {
            Cpu &c = cpu_[i];
            if (c.debt <= 0) continue;
            int slice = c.debt < kSliceCycles ? (int)c.debt : kSliceCycles;
            if (c.inReset) { c.debt -= slice; continue; }
            select(i);
            if (c.needsReset) {
                m68k_pulse_reset();
                c.needsReset = false;
            }
            applyIrq(i);
            int used = m68k_execute(slice);
            c.debt -= used > 0 ? used : slice;
            if (i == 1) {                   // the C139 is the slave's
                c139_.advance(used > 0 ? used : slice);
                c139Interrupt();
            }
        }
    }
}

// ---- interrupts (C148) ----------------------------------------------------------------

void Machine::setLine(int cpu, int level, int state)
{
    if (level <= 0 || level > 7) return;
    cpu_[cpu].line[level] = (uint8_t)state;
    if (cpu == current_) applyIrq(cpu);
}

// The CPU's interrupt level: the highest line asserted. Musashi samples it at
// the start of each slice and when the SR changes (RTE, MOVE to SR...).
void Machine::applyIrq(int cpu)
{
    if (cpu != current_) return;
    int level = 0;
    for (int l = 7; l > 0; l--) {
        if (cpu_[cpu].line[l]) { level = l; break; }
    }
    m68k_set_irq(level);
}

int Machine::intAck(int cpu, int level)
{
    if (level > 0 && level < 8 && cpu_[cpu].line[level] == kHold) {
        cpu_[cpu].line[level] = kClear;
        applyIrq(cpu);
    }
    return M68K_INT_ACK_AUTOVECTOR;
}

void Machine::resetC148(int cpu)
{
    C148 &c = c148_[cpu];
    for (uint8_t *lv : { &c.vblankLevel, &c.posLevel, &c.cpuLevel, &c.exLevel, &c.sciLevel }) {
        setLine(cpu, *lv, kClear);
        *lv = 0;
    }
}

uint16_t Machine::c148Read(int cpu, uint32_t o)
{
    C148 &c = c148_[cpu];
    switch (o >> 13) {
    case 0x04000 >> 13: return c.bus;
    case 0x06000 >> 13: return c.cpuLevel;
    case 0x08000 >> 13: return c.exLevel;
    case 0x0a000 >> 13: return c.posLevel;
    case 0x0c000 >> 13: return c.sciLevel;
    case 0x0e000 >> 13: return c.vblankLevel;
    case 0x16000 >> 13: setLine(cpu, c.cpuLevel, kClear); return 0;
    case 0x18000 >> 13: setLine(cpu, c.exLevel, kClear); return 0;
    case 0x1a000 >> 13: setLine(cpu, c.posLevel, kClear); return 0;
    case 0x1c000 >> 13: setLine(cpu, c.sciLevel, kClear); return 0;
    case 0x1e000 >> 13: setLine(cpu, c.vblankLevel, kClear); return 0;
    case 0x20000 >> 13: return 7;       // EEPROM ready (master)
    default: return 0;
    }
}

void Machine::c148Write(int cpu, uint32_t o, uint16_t data)
{
    C148 &c = c148_[cpu];
    uint8_t v = data & 7;
    auto level = [&](uint8_t &lv) { setLine(cpu, lv, kClear); lv = v; };
    switch (o >> 13) {
    case 0x04000 >> 13: c.bus = v; break;
    case 0x06000 >> 13: level(c.cpuLevel); break;
    case 0x08000 >> 13: level(c.exLevel); break;
    case 0x0a000 >> 13: level(c.posLevel); break;
    case 0x0c000 >> 13: level(c.sciLevel); break;
    case 0x0e000 >> 13: level(c.vblankLevel); break;
    case 0x10000 >> 13: {               // interrupt the other CPU
        int other = cpu ^ 1;
        setLine(other, c148_[other].cpuLevel, kAssert);
        break;
    }
    case 0x16000 >> 13: setLine(cpu, c.cpuLevel, kClear); break;
    case 0x18000 >> 13: setLine(cpu, c.exLevel, kClear); break;
    case 0x1a000 >> 13: setLine(cpu, c.posLevel, kClear); break;
    case 0x1c000 >> 13: setLine(cpu, c.sciLevel, kClear); break;
    case 0x1e000 >> 13: setLine(cpu, c.vblankLevel, kClear); break;
    case 0x22000 >> 13:                 // sound CPU reset (master only)
        if (cpu == 0) sound_.reset(!(data & 1));
        break;
    case 0x24000 >> 13:                 // slave 68000, I/O MCU and DSPs reset (master only)
        if (cpu == 0) subcpuReset(!(data & 1));
        break;
    default: break;                     // watchdog, bus acknowledge
    }
}

// MAME's reset_all_subcpus.
void Machine::subcpuReset(bool assert)
{
    if (assert == subcpusHeld_) return;
    subcpusHeld_ = assert;
    Cpu &s = cpu_[1];
    if (assert) {
        s.inReset = true;
        resetC148(1);
        memset(s.line, 0, sizeof s.line);
        dsp_.reset();
    } else {
        // Called from inside the master's write: the slave's reset waits for
        // the scheduler (switching CPU contexts mid-instruction is not safe).
        s.inReset = false;
        s.needsReset = true;
        mcu_.reset(dpram_);
    }
}

// ---- the memory map ------------------------------------------------------------------

static uint16_t romWord(const std::vector<uint8_t> &rom, uint32_t a)
{
    return a + 1 < rom.size() ? (uint16_t)(rom[a] << 8 | rom[a + 1]) : 0;
}

static void combine(uint16_t &dst, uint16_t data, uint16_t mask)
{
    dst = (uint16_t)((dst & ~mask) | (data & mask));
}

uint16_t Machine::read16(int cpu, uint32_t a)
{
    if (cpu == 0) {
        if (a < 0x100000) return romWord(roms_.main, a);
        if (a >= 0x100000 && a < 0x110000) return mainRam_[(a - 0x100000) >> 1];
        if (a >= 0x180000 && a < 0x184000) return nvram_[(a - 0x180000) >> 1];
    } else {
        if (a < 0x80000) return romWord(roms_.slave, a);
        if (a >= 0x100000 && a < 0x140000) return slaveRam_[(a - 0x100000) >> 1];
    }
    if (a >= 0x1c0000 && a < 0x200000) return c148Read(cpu, a - 0x1c0000);
    if (a >= 0x200000 && a < 0x210000) return dspram_[(a - 0x200000) >> 1];
    if (a == 0x440000) return pointram_[pointramIdx_];
    if (a >= 0x480000 && a < 0x480800) return depthcue_[(pointramControl_ & 0x20) ? 1 : 0][(a - 0x480000) >> 1];
    if (a >= 0x700000 && a < 0x720000) return spriteram_[(a - 0x700000) >> 1];
    if (a >= 0x720000 && a < 0x720008) return spritePos_[(a - 0x720000) >> 1];
    if (a >= 0x740000 && a < 0x750000) return palette_[(a - 0x740000) >> 1];
    if (a >= 0x750000 && a < 0x760000) return paletteExt_[(a - 0x750000) >> 1];
    if (a == 0x760000) return videoEnable_;
    if (a >= 0x800000 && a < 0x900000) return romWord(roms_.data, a - 0x800000);
    if (a >= 0x900000 && a < 0x910000) return sharedRam_[(a - 0x900000) >> 1];
    if (a >= 0xa00000 && a < 0xa01000) return dpram_[(a - 0xa00000) >> 1];
    if (a >= 0xb00000 && a < 0xb04000) return c139Ram_[(a - 0xb00000) >> 1];
    if (a >= 0xb80000 && a < 0xb80010) return c139_.read((int)(a - 0xb80000) >> 1);   // C139 registers
    if (a >= 0xc00000 && a < 0xe00000) return romWord(roms_.edata, (a - 0xc00000) & 0xfffff);   // 1 MB, twice
    return 0;
}

void Machine::write16(int cpu, uint32_t a, uint16_t data, uint16_t mask)
{
    if (cpu == 0) {
        if (a >= 0x100000 && a < 0x110000) { combine(mainRam_[(a - 0x100000) >> 1], data, mask); return; }
        if (a >= 0x180000 && a < 0x184000) {
            if (mask & 0x00ff) nvram_[(a - 0x180000) >> 1] = data & 0xff;
            return;
        }
    } else {
        if (a >= 0x100000 && a < 0x140000) { combine(slaveRam_[(a - 0x100000) >> 1], data, mask); return; }
    }
    if (a >= 0x1c0000 && a < 0x200000) {
        if (mask & 0x00ff) c148Write(cpu, a - 0x1c0000, data);
        else c148Write(cpu, a - 0x1c0000, data >> 8);
        return;
    }
    if (a >= 0x200000 && a < 0x210000) {
        int w = (a - 0x200000) >> 1;
        combine(dspram_[w], data, mask);
        dsp_.write(dspram_, w);
        return;
    }
    if (a == 0x400000) { combine(pointramControl_, data, mask); pointramIdx_ = 0; return; }
    if (a == 0x440000) {
        if (mask & 0x00ff) {
            pointram_[pointramIdx_] = data & 0xff;
            pointramIdx_ = (pointramIdx_ + 1) & 0x1ffff;
        }
        return;
    }
    if (a >= 0x480000 && a < 0x480800) {
        if (mask & 0x00ff) depthcue_[(pointramControl_ & 0x20) ? 1 : 0][(a - 0x480000) >> 1] = data & 0xff;
        return;
    }
    if (a >= 0x700000 && a < 0x720000) { spriteramWrite((a - 0x700000) >> 1, data, mask); return; }
    if (a >= 0x720000 && a < 0x720008) { combine(spritePos_[(a - 0x720000) >> 1], data, mask); return; }
    if (a >= 0x740000 && a < 0x750000) { combine(palette_[(a - 0x740000) >> 1], data, mask); return; }
    if (a >= 0x750000 && a < 0x760000) { combine(paletteExt_[(a - 0x750000) >> 1], data, mask); return; }
    if (a == 0x760000) { combine(videoEnable_, data, mask); return; }
    if (a >= 0x900000 && a < 0x910000) { combine(sharedRam_[(a - 0x900000) >> 1], data, mask); return; }
    if (a >= 0xa00000 && a < 0xa01000) {
        if (mask & 0x00ff) {
            int o = (a - 0xa00000) >> 1;
            uint8_t v = data & 0xff;
            dpram_[o] = v;
            if (o < 0x100) mcu_.write(dpram_, o, v);   // (the sound board reads its part itself)
        }
        return;
    }
    if (a >= 0xb00000 && a < 0xb04000) { combine(c139Ram_[(a - 0xb00000) >> 1], data, mask); return; }
    if (a >= 0xb80000 && a < 0xb80010) {
        c139_.write((int)(a - 0xb80000) >> 1, data, mask);
        c139Interrupt();
        return;
    }
}

// The C139's interrupt: the slave's, at the level its C148 gives serial
// (Cyber Sled: 4), until the game acknowledges it.
void Machine::c139Interrupt()
{
    if (c139_.takeInterrupt() && c148_[1].sciLevel) setLine(1, c148_[1].sciLevel, kAssert);
}

// The C355 mirrors two windows of its RAM into the first one (MAME's spriteram_w).
void Machine::spriteramWrite(uint32_t o, uint16_t data, uint16_t mask)
{
    combine(spriteram_[o], data, mask);
    if (o >= 0x10000 / 2 && o < 0x12000 / 2) combine(spriteram_[o - 0x10000 / 2], data, mask);
    else if (o >= 0x14000 / 2 && o < 0x14400 / 2) combine(spriteram_[o - 0x12000 / 2], data, mask);
}

// xBRG_888 over two RAMs: main word = R << 8 | G, ext word low byte = B.
uint32_t Machine::paletteRgb(int index) const
{
    index &= 0x7fff;
    uint32_t r = palette_[index] >> 8, g = palette_[index] & 0xff, b = paletteExt_[index] & 0xff;
    return r << 16 | g << 8 | b;
}

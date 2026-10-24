// Namco System 21 games for VectorPie (Star Blade, Cyber Sled: profile.cpp):
// the game's own 68000 code running on Musashi, its 3D and sprites drawn as
// vectors, its sound board running its own program (soundboard.cpp). The
// package's game.ini says which game. Needs MAME's ROM set of it (starblad.zip,
// cybsled.zip; the cabinet's ROM folders are searched).
//
//   system21_emu [--roms PATH] [--game SET] [--dvg [PORT] | --window | --windowed | --maximized]
//   (--game cybsled: another game than game.ini's, from a build folder)
//   TAB, or START 1 and START 2 together: the CONTROLS page (the player's own
//   controls for the game's functions, kept in save/controls.ini;
//   the aim speed, in save/options.ini)
//   P (ui_pause): pause / resume
//
// Development tools, with S21_DEV 1 (below):
//   --sprite-ids: sprites not traced yet drawn as a box with their ID
//   F5: save the game's state (save/state.bin); F7: load it, at any time,
//   e.g. right after starting: back to that moment
//   --record SECONDS: load save/state.bin, then for SECONDS write a capture
//               every RECORD_EVERY frames (save/record_<frame>.png at half
//               size, and .txt: the sprites), then quit
//   P: pausing also saves save/capture_<frame>.png (and .bin, .txt: the sprites, .pal: the palette, _sound.txt: the sound board)
//   --dump DIR: for comparing with MAME captures: the DSP RAM
//               every 300 frames (sbdump_dspram_<frame>.bin, 64 KB big-endian)
//               and trace.txt (each frame: page select, records drawn by type)
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <vectorpie/vp.h>

#include "hle.h"
#include "link.h"
#include "machine.h"
#include "profile.h"
#include "render3d.h"
#include "roms.h"
#include "soundboard.h"
#include "state.h"
#include "sprites.h"

namespace {

// The development tools: F5 / F7 save states, --record, --dump,
// --sprite-ids and the pause capture (P still pauses). 0 for the game as
// shipped; 1 to have them back. Kept compiled either way.
#define S21_DEV 0
constexpr bool DEV = S21_DEV != 0;

const long BOOT_FRAMES = 5000;          // the self test is skipped up to here at most
const int RECORD_EVERY = 6;             // --record: a capture every 6 frames, 10 a second

// The functions a player lays out (vp_controls.h): the game's (profile.h),
// out of the box the cabinet's.
int nfunctions() { return (int)profile().nfunctions; }
const GameFunction &function(int i) { return profile().functions[i]; }

// The CONTROLS page's setting, after the functions: the aim speed (the
// stick's range, the aim keys' and the mouse's pace). Then EXIT. (No dead
// zone: the stick counts from its first movement, as on the arcade.)
struct AimSpeed { const char *name; float k; };
const AimSpeed SPEEDS[] = { { "SLOW", 0.6f }, { "NORMAL", 1.0f }, { "FAST", 1.5f }, { "FASTER", 2.0f } };
const int SPEED_STANDARD = 1;
// A game that links two cabinets (profile.h) has a LINK row too: OFF or ON.
bool canLink() { return profile().linkNvram >= 0; }
int rowSpeed() { return nfunctions(); }
int rowLink() { return canLink() ? nfunctions() + 1 : -1; }
int rowExit() { return nfunctions() + 1 + (canLink() ? 1 : 0); }
int rows() { return rowExit() + 1; }
const int KEY_TAB = 43, KEY_DELETE = 76, KEY_KP_PERIOD = 99, KEY_F5 = 62, KEY_F7 = 64;   // SDL scancodes

// Development (S21_DEV 1): --script FILE, presses at given frames, for
// testing what needs players (two linked copies...). Lines "<frame> <action>":
// coin, start, gun, missile (held 10 frames), up/down/left/right (stick 1,
// held 20), up2/down2/left2/right2 (stick 2), shot (the picture,
// save/shot_<frame>.png).
struct ScriptStep { long frame; std::string action; };
std::vector<ScriptStep> g_script;
std::vector<long> g_shots;

struct Game {
    std::string romPath;
    bool spriteIds = false;
    bool paused = false;
    std::string dumpDir;
    FILE *trace = nullptr;
    std::string error;
    Roms roms;
    Mcu mcu;
    Sound sound;
    Dsp dsp;
    std::unique_ptr<Machine> machine;
    std::unique_ptr<Render3d> render3d;
    std::unique_ptr<Sprites> sprites;
    Link link;                          // to another cabinet (LINK ON: docs/link.md)
    bool linkOn = false;
    // The CONTROLS page: open (the game stands still), the cursor's row
    // (the aim speed, then EXIT, after the functions), a function being laid out,
    // what the page says; the aim speed.
    bool controls = false;
    int ctlRow = 0;
    int speed = SPEED_STANDARD;         // index in SPEEDS
    int yokeWay = 0;                    // the yoke's last way on the CONTROLS page: 1 down, -1 up, 0 centered
    vp_capture capture{};
    std::string said;
    bool holdStarts = false;            // after the page: START is not the game's until let go
    double recordSeconds = 0;           // --record: how long; 0: not recording
    long recordStart = -1;              // the frame it started at (after loading the state)
    std::string stateSaid;              // "STATE SAVED" ..., shown for a while
    double stateSaidUntil = 0;
};

// ---- save states (F5 / F7) -------------------------------------------------------

const char STATE_MAGIC[8] = { 'S', 'B', 'E', 'M', 'U', 'S', 'T', '1' };

std::string statePath()
{
    return std::string(vp_save_dir()) + "/state.bin";
}

void say(Game &s, vp_game *g, const char *what)
{
    s.stateSaid = what;
    s.stateSaidUntil = vp_game_time(g) + 2.0;
}

void saveState(Game &s, vp_game *g)
{
    State st(true);
    st.bytes(const_cast<char *>(STATE_MAGIC), sizeof STATE_MAGIC);
    s.machine->state(st);
    FILE *f = fopen(statePath().c_str(), "wb");
    bool ok = f && fwrite(st.data().data(), 1, st.data().size(), f) == st.data().size();
    if (f && fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "system21: cannot write %s\n", statePath().c_str());
    say(s, g, ok ? "STATE SAVED" : "STATE NOT SAVED");
}

void loadState(Game &s, vp_game *g)
{
    std::vector<uint8_t> data;
    if (FILE *f = fopen(statePath().c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    if (data.size() < sizeof STATE_MAGIC || memcmp(data.data(), STATE_MAGIC, sizeof STATE_MAGIC)) {
        say(s, g, data.empty() ? "NO SAVED STATE" : "NOT A STATE OF THIS VERSION");
        return;
    }
    // The machine as it is now, to put back if the file does not fit.
    State now(true);
    s.machine->state(now);
    State st(std::move(data));
    char magic[sizeof STATE_MAGIC];
    st.bytes(magic, sizeof magic);
    s.machine->state(st);
    if (!st.ok() || !st.atEnd()) {
        State back(std::move(now.data()));
        s.machine->state(back);
        say(s, g, "NOT A STATE OF THIS VERSION");
        return;
    }
    say(s, g, "STATE LOADED");
}

void saveControls(vp_input *in)
{
    if (vp_input_save(in, nullptr) != VP_OK) fprintf(stderr, "system21: controls not saved: %s\n", vp_last_error());
}

// The setting: applied, and kept in save/options.ini ("aim_speed = FAST").
std::string optionsPath() { return std::string(vp_save_dir()) + "/options.ini"; }

void applyOptions(Game &s, vp_input *in)
{
    vp_input_set_deadzone(in, 0.0f);
    s.mcu.setAimSpeed(SPEEDS[s.speed].k);
    // The link: looking for the other cabinet while ON.
    const bool want = s.linkOn && canLink();
    if (want && !s.link.isOpen()) {
        if (!s.link.open(profile().set)) fprintf(stderr, "system21: link: %s\n", s.link.error().c_str());
    } else if (!want && s.link.isOpen()) {
        s.link.close();
    }
}

void saveOptions(Game &s)
{
    FILE *f = fopen(optionsPath().c_str(), "w");
    if (!f) { fprintf(stderr, "system21: cannot write %s\n", optionsPath().c_str()); return; }
    fprintf(f, "# %s: the CONTROLS page's settings\naim_speed = %s\n", profile().title, SPEEDS[s.speed].name);
    if (canLink()) fprintf(f, "link = %s\n", s.linkOn ? "ON" : "OFF");
    fclose(f);
}

void loadOptions(Game &s)
{
    FILE *f = fopen(optionsPath().c_str(), "r");
    if (!f) return;
    char line[128], value[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " aim_speed = %63s", value) == 1) {
            for (int i = 0; i < (int)(sizeof SPEEDS / sizeof SPEEDS[0]); i++)
                if (!strcmp(SPEEDS[i].name, value)) s.speed = i;
        }
        if (sscanf(line, " link = %63s", value) == 1) s.linkOn = !strcmp(value, "ON");
    }
    fclose(f);
}

void openControls(Game &s, vp_game *g)
{
    s.controls = true;
    s.ctlRow = 0;
    s.capture = vp_capture{};
    s.said.clear();
    vp_audio_set_master(vp_game_audio(g), 0.0f);
}

void closeControls(Game &s, vp_game *g)
{
    s.controls = false;
    s.holdStarts = true;
    vp_audio_set_master(vp_game_audio(g), 1.0f);
}

// The CONTROLS page: Up/Down a row, START 1 / Enter lays out its function's
// controls (vp_capture), Left/Right none or standard, Delete standard; on
// AIM SPEED Left/Right change it, Delete its standard; EXIT or TAB back to
// the game.
void controlsPage(Game &s, vp_game *g)
{
    vp_input *in = vp_game_input(g);
    if (s.capture.capturing) {
        if (vp_capture_step(&s.capture, in) == VP_CAPTURE_CHANGED) saveControls(in);
    } else if (vp_input_key_pressed(in, KEY_TAB)) {
        closeControls(s, g);
        return;
    } else {
        // Up and down the list. A flight yoke goes the other way: pulled
        // back (rotated down) it aims up in play, but on a list "down" is
        // down. While the yoke is out, it alone moves the list, turned
        // (the SDK's menu reading would take its raw way); the keys, hats
        // and other sticks as usual. Only here: in play nothing changes.
        bool up = vp_input_menu_up(in), down = vp_input_menu_down(in);
        const int yoke = vp_input_yoke(in);
        const float yy = yoke >= 0 ? vp_input_joy_axis(in, yoke, 1) : 0.0f;
        const int way = yy < -0.5f ? 1 : yy > 0.5f ? -1 : 0;
        if (way || s.yokeWay) {
            up = way == -1 && s.yokeWay != -1;
            down = way == 1 && s.yokeWay != 1;
        }
        s.yokeWay = way;
        if (up || down) {
            s.ctlRow = (s.ctlRow + (up ? rows() - 1 : 1)) % rows();
            vp_capture_moved(&s.capture);
        }
        int left = vp_input_menu_left_pressed(in), right = vp_input_menu_right_pressed(in);
        vp_capture_filter(&s.capture, &left, &right);
        if (s.ctlRow < nfunctions()) {
            const char *name = function(s.ctlRow).name;
            if (vp_input_menu_confirm(in)) {
                vp_capture_begin(&s.capture, in, name);
            } else if (left || right) {
                vp_capture_toggle(&s.capture, in, name);
                saveControls(in);
            } else if (vp_input_key_pressed(in, KEY_DELETE) || vp_input_key_pressed(in, KEY_KP_PERIOD)) {
                vp_capture_standard(&s.capture, in, name);
                saveControls(in);
            }
        } else if (s.ctlRow == rowSpeed()) {
            const int count = (int)(sizeof SPEEDS / sizeof SPEEDS[0]), was = s.speed;
            if (left && s.speed > 0) s.speed--;
            if (right && s.speed < count - 1) s.speed++;
            if (vp_input_key_pressed(in, KEY_DELETE) || vp_input_key_pressed(in, KEY_KP_PERIOD))
                s.speed = SPEED_STANDARD;
            if (s.speed != was) { applyOptions(s, in); saveOptions(s); }
        } else if (s.ctlRow == rowLink()) {
            const bool was = s.linkOn;
            if (left || right) s.linkOn = !s.linkOn;
            if (vp_input_key_pressed(in, KEY_DELETE) || vp_input_key_pressed(in, KEY_KP_PERIOD))
                s.linkOn = false;
            if (s.linkOn != was) { applyOptions(s, in); saveOptions(s); }
        } else if (vp_input_menu_confirm(in)) {
            closeControls(s, g);
            return;
        }
    }
    const char *said = vp_capture_said(&s.capture);
    s.said = said ? said : "";
}

// A row per function, its controls beside it (gray while they are the
// standard), the row being laid out blinking; EXIT; the help.
void drawControls(const Game &s, vp_game *g, vp_dvg *d)
{
    vp_input *in = vp_game_input(g);
    char text[VP_CONTROLS_LENGTH * 2];
    vp_text_font(VP_FONT_ASTEROIDS);
    vp_color(d, 0, 200, 255);
    vp_text(d, "CONTROLS", 2048, 3450, 170, VP_TEXT_CENTER);
    // Star Blade's 8 rows fill the page; more (Cyber Sled's 12) are squeezed
    // into the same height, their letters a little smaller (at least 80%).
    const float span = 7 * 250.0f + 120.0f;
    const float fit = std::min(1.0f, span / ((rows() - 1) * 250.0f + 120.0f));
    const float step = 250.0f * fit, gap = 60.0f * fit, size = std::max(fit, 0.8f);
    for (int i = 0; i < rows(); i++) {
        const bool sel = i == s.ctlRow;
        const float y = 3050 - i * step - (i >= rowSpeed() ? gap : 0.0f) - (i == rowExit() ? gap : 0.0f);
        vp_text_font(VP_FONT_ASTEROIDS);
        if (sel) vp_color(d, 255, 60, 60); else vp_color(d, 255, 255, 255);
        if (i == rowExit()) {
            vp_text(d, "EXIT", 2048, y, 100 * size, VP_TEXT_CENTER);
            break;
        }
        if (i == rowSpeed()) {
            vp_text(d, "AIM SPEED", 450, y, 90 * size, VP_TEXT_LEFT);
            if (s.speed == SPEED_STANDARD) vp_color(d, 150, 150, 150);
            else if (!sel) vp_color(d, 120, 255, 120);
            vp_text(d, SPEEDS[s.speed].name, 1900, y, 90 * size, VP_TEXT_LEFT);
            continue;
        }
        if (i == rowLink()) {
            // OFF, or ON and how it stands: looking, linked as cabinet 1 or 2
            vp_text(d, "LINK", 450, y, 90 * size, VP_TEXT_LEFT);
            const char *state = !s.linkOn ? "OFF"
                              : !s.link.isOpen() ? "NO NETWORK"
                              : !s.link.paired() ? "SEARCHING"
                              : s.link.cabinet() == 0 ? "CABINET 1" : "CABINET 2";
            if (!s.linkOn) vp_color(d, 150, 150, 150);
            else if (!sel) vp_color(d, 120, 255, 120);
            vp_text(d, state, 1900, y, 90 * size, VP_TEXT_LEFT);
            continue;
        }
        vp_text(d, function(i).label, 450, y, 90 * size, VP_TEXT_LEFT);
        if (sel && s.capture.capturing) {
            if ((int)(vp_game_time(g) * 3) % 2 == 0) {
                vp_color(d, 255, 220, 60);
                vp_text(d, "...", 1900, y, 90 * size, VP_TEXT_LEFT);
            }
            continue;
        }
        const char *has = vp_input_controls(in, function(i).name);
        vp_input_spelled(in, function(i).name, has, text, sizeof text);
        vp_text_font(VP_FONT_HERSHEY);
        if (has && !strcmp(has, "standard")) vp_color(d, 150, 150, 150);
        else if (!sel) vp_color(d, 120, 255, 120);
        vp_text(d, text, 1900, y, 75 * size, VP_TEXT_LEFT);
    }
    vp_text_font(VP_FONT_HERSHEY);
    if (!s.said.empty()) {
        vp_color(d, 255, 220, 60);
        vp_text(d, s.said.c_str(), 2048, 900, 75, VP_TEXT_CENTER);
    }
    vp_color(d, 160, 160, 160);
    if (s.ctlRow == rowSpeed()) {
        vp_text(d, "LEFT, RIGHT - CHANGE   DEL - STANDARD", 2048, 620, 75, VP_TEXT_CENTER);
    } else if (s.ctlRow == rowLink()) {
        vp_text(d, "LEFT, RIGHT - ON OR OFF: ON ON BOTH CABINETS", 2048, 680, 75, VP_TEXT_CENTER);
        vp_text(d, "THEY FIND EACH OTHER ON THE NETWORK", 2048, 550, 75, VP_TEXT_CENTER);
    } else if (s.ctlRow < nfunctions() || s.capture.capturing) {
        const char *one, *two;
        vp_capture_help(&s.capture, &one, &two);
        vp_text(d, one, 2048, 680, 75, VP_TEXT_CENTER);
        vp_text(d, two, 2048, 550, 75, VP_TEXT_CENTER);
    } else {
        vp_text(d, "START, ENTER OR TAB - BACK TO THE GAME", 2048, 620, 75, VP_TEXT_CENTER);
    }
}

std::string nvramPath()
{
    return std::string(vp_save_dir()) + "/" + profile().set + ".nv";   // starblad.nv
}

void loadNvram(std::vector<uint8_t> &nv)
{
    FILE *f = fopen(nvramPath().c_str(), "rb");
    if (!f) return;
    std::vector<uint8_t> data(nv.size());
    if (fread(data.data(), 1, data.size(), f) == data.size()) nv = data;
    fclose(f);
}

void saveNvram(const std::vector<uint8_t> &nv)
{
    FILE *f = fopen(nvramPath().c_str(), "wb");
    if (!f) return;
    fwrite(nv.data(), 1, nv.size(), f);
    fclose(f);
}

// --dump: what our DSP stand-in sees, to compare with MAME's captures.
void dump(Game &s)
{
    const long f = s.machine->frame();
    const uint16_t *ram = s.machine->dspram();
    if (!s.trace) s.trace = fopen((s.dumpDir + "/trace.txt").c_str(), "w");
    if (s.trace) {
        const Dsp::Frame &fr = s.dsp.finished();
        int counts[4] = {};             // types 0, 1/2, 100h/101h, other
        for (size_t i = 0; i < fr.list.size() && fr.list[i] != 0xffff;) {
            uint16_t t = fr.list[i];
            size_t len = (t == 1 || t == 2) ? 13 : t == 0 ? 6 : t == 0x100 ? 11 : t == 0x101 ? 14 : 0;
            counts[t == 0 ? 0 : (t == 1 || t == 2) ? 1 : len ? 2 : 3]++;
            if (!len) break;
            i += len;
        }
        fprintf(s.trace, "%ld sel=%d valid=%d type0=%d type1/2=%d type100=%d bad=%d\n", f, s.machine->dspram()[0x103],
                fr.valid ? 1 : 0, counts[0], counts[1], counts[2], counts[3]);
        if (f % 300 == 0) fflush(s.trace);
    }
    if (f % 300 == 0) {
        char name[64];
        snprintf(name, sizeof name, "/sbdump_dspram_%06ld.bin", f);
        FILE *out = fopen((s.dumpDir + name).c_str(), "wb");
        if (!out) return;
        for (int i = 0; i < 0x8000; i++) {
            uint8_t b[2] = { (uint8_t)(ram[i] >> 8), (uint8_t)ram[i] };
            fwrite(b, 1, 2, out);
        }
        fclose(out);
    }
}

// --record's: the picture at half size and the sprite list.
void recordFrame(Game &s, vp_dvg *d)
{
    char name[64];
    snprintf(name, sizeof name, "/record_%06ld", s.machine->frame());
    std::string base = std::string(vp_save_dir()) + name;
    if (vp_dvg_save_png(d, (base + ".png").c_str(), 512, 384) != VP_OK)
        fprintf(stderr, "system21: %s\n", vp_last_error());
    if (FILE *out = fopen((base + ".txt").c_str(), "w")) {
        s.sprites->list(out, *s.machine);
        fclose(out);
    }
}

void capture(Game &s, vp_dvg *d)
{
    char name[64];
    snprintf(name, sizeof name, "/capture_%06ld", s.machine->frame());
    std::string base = std::string(vp_save_dir()) + name;
    if (vp_dvg_save_png(d, (base + ".png").c_str(), 1024, 768) != VP_OK)
        fprintf(stderr, "system21: %s\n", vp_last_error());
    FILE *out = fopen((base + ".bin").c_str(), "wb");
    if (!out) return;
    const uint16_t *ram = s.machine->dspram();
    for (int i = 0; i < 0x8000; i++) {
        uint8_t b[2] = { (uint8_t)(ram[i] >> 8), (uint8_t)ram[i] };
        fwrite(b, 1, 2, out);
    }
    fclose(out);
    if ((out = fopen((base + ".txt").c_str(), "w"))) {
        s.sprites->list(out, *s.machine);
        fclose(out);
    }
    if ((out = fopen((base + "_sound.txt").c_str(), "w"))) {
        s.sound.list(out);
        fclose(out);
    }
    // The palette, as the game has it now: 32768 colors, r g b bytes.
    if ((out = fopen((base + ".pal").c_str(), "wb"))) {
        for (int i = 0; i < 0x8000; i++) {
            uint32_t rgb = s.machine->paletteRgb(i);
            uint8_t b[3] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
            fwrite(b, 1, 3, out);
        }
        fclose(out);
    }
}

int init(vp_game *g)
{
    Game &s = *(Game *)vp_game_user(g);
    if (!roms_load(s.romPath, profile(), s.roms, s.error)) {
        fprintf(stderr, "system21: %s\n", s.error.c_str());
        return VP_OK;                   // shown on screen, then back to the menu
    }
    s.machine = std::make_unique<Machine>(s.roms, s.mcu, s.sound, s.dsp);
    s.machine->setLink(&s.link);
    loadNvram(s.machine->nvram());
    s.render3d = std::make_unique<Render3d>();
    if (s.render3d->open(s.roms.points) != VP_OK) {
        fprintf(stderr, "system21: %s\n", vp_last_error());
        return VP_ERR_NOMEM;
    }
    s.sprites = std::make_unique<Sprites>(s.roms.sprites);
    // The traces: the package's sprites.vec. In a build folder every game's
    // are in games/<set>/ too (--game: another game than game.ini's).
    char vec[1024];
    const std::string own = std::string("games/") + profile().set + "/sprites.vec";
    vp_asset_path(own.c_str(), vec, sizeof vec);
    if (FILE *f = fopen(vec, "r")) fclose(f);
    else vp_asset_path("sprites.vec", vec, sizeof vec);
    s.sprites->load(vec);
    s.sprites->setShowIds(DEV && s.spriteIds);
    vp_input *in = vp_game_input(g);
    for (int i = 0; i < nfunctions(); i++) vp_input_define(in, function(i).name, function(i).standard);
    vp_input_reserve_key(in, KEY_TAB);          // opens and closes the CONTROLS page
    if (DEV) {
        vp_input_reserve_key(in, KEY_F5);       // save state
        vp_input_reserve_key(in, KEY_F7);       // load state
    }
    vp_input_load(in, nullptr);                 // the player's own, if they set any
    loadOptions(s);                             // the aim speed
    applyOptions(s, in);
    s.mcu.setInput(in);
    // The mouse aims (and its left button fires): captured, so it keeps
    // counting at the window's edge.
    vp_input_grab_mouse(vp_game_input(g), 1);
    s.sound.open(vp_game_audio(g), s.roms, s.machine->dpram());
    s.machine->reset();
    return VP_OK;
}

// Linked: this cabinet's number (the link's: 0 or 1) in the game's NVRAM,
// with its settings' checksum. The game reads it at boot: a change restarts
// it (the boot is not shown).
void linkCabinet(Game &s)
{
    const int at = profile().linkNvram;
    if (at < 0 || !s.link.paired()) return;
    std::vector<uint8_t> &nv = s.machine->nvram();
    if ((size_t)at >= nv.size() || (nv[at] & 1) == s.link.cabinet()) return;
    nv[at] = (uint8_t)((nv[at] & ~1) | s.link.cabinet());
    const int n = profile().settingsSum;
    if (n > 0 && (size_t)n < nv.size()) {
        unsigned sum = 0;
        for (int i = 0; i < n; i++) sum += nv[i];
        nv[n] = (uint8_t)sum;
    }
    saveNvram(nv);
    s.machine->powerOn();
    s.dsp.clearStarts();
}

// Still in the boot self test: until the game has started its DSPs as many
// times as its profile says, BOOT_FRAMES at most.
bool booting(const Game &s)
{
    return s.machine->frame() < BOOT_FRAMES && s.dsp.starts() < profile().bootStarts;
}

void update(vp_game *g, float)
{
    Game &s = *(Game *)vp_game_user(g);
    if (!s.machine) {
        if (vp_game_time(g) > 8.0) vp_game_quit(g);
        return;
    }
    // The link (LINK ON): the network, then this cabinet's number.
    s.link.poll();
    linkCabinet(s);
    // The CONTROLS page: the game stands still while it is open. TAB, or
    // START 1 and START 2 together, opens it.
    vp_input *in = vp_game_input(g);
    if (s.controls) {
        controlsPage(s, g);
        return;
    }
    if (!s.paused && (vp_input_key_pressed(in, KEY_TAB) ||
                      (vp_input_held(in, "start1") && vp_input_held(in, "start2") &&
                       (vp_input_pressed(in, "start1") || vp_input_pressed(in, "start2"))))) {
        openControls(s, g);
        return;
    }
    // --record: the saved state, then a capture every RECORD_EVERY frames.
    if (DEV && s.recordSeconds > 0) {
        if (s.recordStart < 0) {
            loadState(s, g);
            if (s.stateSaid != "STATE LOADED") {
                fprintf(stderr, "system21: --record: %s (%s)\n", s.stateSaid.c_str(), statePath().c_str());
                vp_game_quit(g);
                return;
            }
            s.recordStart = s.machine->frame();
        } else {
            const long done = s.machine->frame() - s.recordStart;
            if (done % RECORD_EVERY == 0) recordFrame(s, vp_game_output(g));
            if (done >= (long)(s.recordSeconds * Machine::kFrameRate)) { vp_game_quit(g); return; }
        }
    }
    if (DEV && vp_input_key_pressed(in, KEY_F5)) saveState(s, g);
    if (DEV && vp_input_key_pressed(in, KEY_F7)) loadState(s, g);
    // Leaving the page with START: the game does not get that press.
    if (s.holdStarts) {
        if (vp_input_held(in, "start1") || vp_input_held(in, "start2") || vp_input_menu_confirm_held(in)) return;
        s.holdStarts = false;
    }
    // P (ui_pause): freeze the game, to compare a moment with the original.
    // Pausing also saves the frame (the last one sent) and the DSP RAM, into
    // save/: capture_<frame>.png, .bin (the DSP RAM), .txt (the sprite list)
    // and .pal (the palette).
    if (vp_input_pressed(vp_game_input(g), "ui_pause")) {
        s.paused = !s.paused;
        vp_audio_set_master(vp_game_audio(g), s.paused ? 0.0f : 1.0f);
        if (DEV && s.paused) capture(s, vp_game_output(g));
        vp_input_grab_mouse(vp_game_input(g), !s.paused);   // the desktop's while paused
    }
    if (s.paused) return;
    // The boot self test (RAM, ROMs, waiting on the DSPs): run through it
    // at full speed, not shown.
    if (booting(s)) {
        for (int k = 0; k < 100 && booting(s); k++) s.machine->runFrame();
        return;
    }
    if (DEV) {  // --script: its presses this frame
        const long f = s.machine->frame();
        uint8_t b = 0; bool coin = false; float st[2][2] = { { 0, 0 }, { 0, 0 } };
        for (const ScriptStep &e : g_script) {
            const long age = f - e.frame;
            const bool btn = age >= 0 && age < 10, stk = age >= 0 && age < 20;
            if (btn && e.action == "coin") coin = true;
            if (btn && e.action == "start") b |= 0x80;
            if (btn && e.action == "gun") b |= 0x20;
            if (btn && e.action == "missile") b |= 0x10;
            for (int k = 0; k < 2; k++) {
                const std::string sfx = k ? "2" : "";
                if (stk && e.action == "up" + sfx) st[k][1] = 1;
                if (stk && e.action == "down" + sfx) st[k][1] = -1;
                if (stk && e.action == "left" + sfx) st[k][0] = -1;
                if (stk && e.action == "right" + sfx) st[k][0] = 1;
            }
        }
        s.mcu.scriptButtons = b;
        s.mcu.scriptCoin = coin;
        memcpy(s.mcu.scriptStick, st, sizeof st);
    }
    s.machine->runFrame();
    if (DEV && !s.dumpDir.empty()) dump(s);
}

void draw(vp_game *g, vp_dvg *d)
{
    Game &s = *(Game *)vp_game_user(g);
    if (s.machine && booting(s)) return;   // the self test: not shown
    if (!s.machine) {
        vp_color(d, 255, 80, 80);
        // the game's name, its zip, in capitals: STAR BLADE, STARBLAD.ZIP
        std::string title = profile().title, zip = std::string(profile().set) + ".zip";
        for (char &c : title) c = (char)toupper((unsigned char)c);
        for (char &c : zip) c = (char)toupper((unsigned char)c);
        vp_text(d, title.c_str(), 2048, 2500, 160, VP_TEXT_CENTER);
        vp_color(d, 255, 255, 255);
        vp_text(d, (zip + " NOT FOUND").c_str(), 2048, 2000, 110, VP_TEXT_CENTER);
        vp_text(d, ("PUT MAME'S " + title + " ROMS IN THE ROM FOLDER").c_str(), 2048, 1700, 80, VP_TEXT_CENTER);
        return;
    }
    if (s.controls) {
        drawControls(s, g, d);
        return;
    }
    if (s.machine->videoEnabled()) {
        // The sprites first: the HUD's panels they find hide the 3D.
        s.sprites->draw(d, *s.machine);
        s.render3d->draw(d, s.dsp.finished(), *s.machine, s.sprites->hides());
        static size_t nextShot = 0;   // --script: its pictures
        if (nextShot < g_shots.size() && s.machine->frame() >= g_shots[nextShot]) {
            char name[64];
            snprintf(name, sizeof name, "/shot_%06ld.png", s.machine->frame());
            vp_dvg_save_png(d, (std::string(vp_save_dir()) + name).c_str(), 512, 384);
            nextShot++;
        }
    }
    if (!s.stateSaid.empty() && vp_game_time(g) < s.stateSaidUntil) {
        vp_text_font(VP_FONT_ASTEROIDS);
        vp_color(d, 255, 220, 60);
        vp_text(d, s.stateSaid.c_str(), 2048, 3800, 100, VP_TEXT_CENTER);
    }
}

void shutdown(vp_game *g)
{
    Game &s = *(Game *)vp_game_user(g);
    if (s.machine) saveNvram(s.machine->nvram());
    if (s.trace) fclose(s.trace);
    s.sound.close();
}

} // namespace

int main(int argc, char **argv)
{
    static Game game;

    // Our own options; the rest go to vp_run. (The development ones are
    // read even with S21_DEV 0, and then ignored.)
    std::vector<char *> rest;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && !strcmp(argv[i], "--roms") && i + 1 < argc) { game.romPath = argv[++i]; continue; }
        if (i > 0 && !strcmp(argv[i], "--game") && i + 1 < argc) { profile_choose(argv[++i]); continue; }
        if (i > 0 && !strcmp(argv[i], "--script") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r");
            if (f && !DEV) { fclose(f); f = nullptr; }      // a development tool
            if (f) {
                char line[128], act[64];
                long fr;
                while (fgets(line, sizeof line, f))
                    if (sscanf(line, "%ld %63s", &fr, act) == 2) {
                        if (!strcmp(act, "shot")) g_shots.push_back(fr);
                        else g_script.push_back({ fr, act });
                    }
                fclose(f);
            }
            continue;
        }
        if (i > 0 && !strcmp(argv[i], "--sprite-ids")) { game.spriteIds = true; continue; }
        if (i > 0 && !strcmp(argv[i], "--dump") && i + 1 < argc) { game.dumpDir = argv[++i]; continue; }
        if (i > 0 && !strcmp(argv[i], "--record") && i + 1 < argc) { game.recordSeconds = atof(argv[++i]); continue; }
        rest.push_back(argv[i]);
    }

    vp_game_desc desc = {};
    desc.title = profile().title;       // the game: game.ini's [system21] game
    desc.user = &game;
    desc.init = init;
    desc.update = update;
    desc.draw = draw;
    desc.shutdown = shutdown;
    desc.update_hz = 60;
    desc.frame_hz = 60;
    desc.frame_hz_board = 30;
    return vp_run(&desc, (int)rest.size(), rest.data());
}

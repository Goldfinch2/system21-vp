// profile.cpp: the games (profile.h). Star Blade's values are the ones the
// emulator was built on; another game's come from MAME's namcos21_c67.cpp and
// from the game itself. Star Blade first: the one chosen when none is named.
#include "profile.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <vectorpie/vp_paths.h>

namespace {

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// ---- Star Blade (starblad: ST2, World) -------------------------------------------

const RomFile STARBLAD_ROMS[] = {
    { "st2_mpu.mpru",   Region::Main,    0, 2, 0x80000 },
    { "st2_mpl.mprl",   Region::Main,    1, 2, 0x80000 },
    { "st1-sp-u.bin",   Region::Slave,   0, 2, 0x40000 },
    { "st1-sp-l.bin",   Region::Slave,   1, 2, 0x40000 },
    { "st1-data-u.bin", Region::Data,    0, 2, 0x20000 },
    { "st1-data-l.bin", Region::Data,    1, 2, 0x20000 },
    { "st1-obj0.bin",   Region::Sprites, 0, 4, 0x80000 },
    { "st1-obj1.bin",   Region::Sprites, 1, 4, 0x80000 },
    { "st1-obj2.bin",   Region::Sprites, 2, 4, 0x80000 },
    { "st1-obj3.bin",   Region::Sprites, 3, 4, 0x80000 },
    { "st1-snd0.bin",   Region::Sound,   0, 1, 0x20000 },
    { "st1-voi0.bin",   Region::Voices,  0x000000, 1, 0x80000 },
    { "st1-voi1.bin",   Region::Voices,  0x080000, 1, 0x80000 },
    { "st1-voi2.bin",   Region::Voices,  0x100000, 1, 0x80000 },
    { "st1-voi3.bin",   Region::Voices,  0x180000, 1, 0x80000 },
    // the point ROM: each 24-bit word's high, middle and low byte, two banks of 512K
    { "st1-pt0-h.bin",  Region::PointH,  0x00000, 1, 0x80000 },
    { "st1-pt0-u.bin",  Region::PointU,  0x00000, 1, 0x80000 },
    { "st1-pt0-l.bin",  Region::PointL,  0x00000, 1, 0x80000 },
    { "st1-pt1-h.bin",  Region::PointH,  0x80000, 1, 0x80000 },
    { "st1-pt1-u.bin",  Region::PointU,  0x80000, 1, 0x80000 },
    { "st1-pt1-l.bin",  Region::PointL,  0x80000, 1, 0x80000 },
};

// The stick and the trackball aim as they are; the aim keys push the sight
// like a stick (hle.cpp): no button bit.
const GameFunction STARBLAD_FUNCTIONS[] = {
    { "gun",       "GUN",        "p1_button1", 0x20 },
    { "missile",   "GUN 2",      "p1_button2", 0x10 },   // the same trigger, a second button
    { "aim_up",    "AIM UP",     "p1_up",      0 },
    { "aim_down",  "AIM DOWN",   "p1_down",    0 },
    { "aim_left",  "AIM LEFT",   "p1_left",    0 },
    { "aim_right", "AIM RIGHT",  "p1_right",   0 },
};

// The stick aims (the yoke, a stick, the aim keys, the mouse): its Y
// turned, so up aims up (the game's low value aims down: a flight stick).
// All the way over is the game's own calibration, read from its NVRAM (the
// settings record at 150Ch: X min/center/max at 1526h, Y at 1529h; the
// factory one 01/80/FF), so a full push is the screen's edge whatever the
// cabinet's NVRAM holds; MAME's range if it is not valid.
const Stick STARBLAD_STICKS[] = {
    { 0x09, 0x0a, 0x01, 0xff, 0x18, 0xe8, 0x1526, 0x1529, "p1_stickx", "p1_sticky", true, "aim_up", "aim_down", "aim_left", "aim_right", true },
};

// The warp: 24 lengths of one streak (and again); the horizon band.
const CodeRange STARBLAD_STREAKS[] = { { 1297, 1320 }, { 4743, 4766 } };
const CodeRange STARBLAD_BACKDROPS[] = { { 2, 2 }, { 3448, 3448 } };

// The ASCII font at 20h-7Fh (60h-7Fh repeat 20h-3Fh; 5Bh-5Fh are box
// pieces, not letters), then five other fonts, three digit sets and their
// extras.
const FontRange STARBLAD_FONTS[] = {
    { 0x20, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ" },
    { 0x60, " !\"#$%&'()*+,-./0123456789:;<=>?" },
    { 0x194, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&/" },
    { 0x1bb, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&/" },
    { 0x1e2, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&/" },
    { 0x209, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&" },
    { 0x22f, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!" },
    { 0x255, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789." },
    { 0x399, "0123456789" },
    { 0x3a3, "0123456789" },
    { 0x3ad, "0123456789" },
};
// The coordinates' letters after the digit sets (the Red Eye briefing's
// X+ 2913.64 ...): X Y Z, the same slanted, a minus and a plus; the point.
const FontChar STARBLAD_CHARS[] = {
    { 0x3bd, '.' },
    { 0x3b7, 'X' }, { 0x3b8, 'Y' }, { 0x3b9, 'Z' },
    { 0x3ba, 'X' }, { 0x3bb, 'Y' }, { 0x3bc, 'Z' },
    { 0x3bf, '-' }, { 0x3c0, '+' },
};
const FontWord STARBLAD_WORDS[] = {
    { 0x3c1, "OK" },            // the preflight checklist's green OK
};
// The HUD: its marks, frames, labels and the distance.
const CodeRange STARBLAD_HUD[] = { { 0x35e, 0x398 }, { 0x3be, 0x3be }, { 0x3c2, 0x3c2 } };

// ---- Cyber Sled (cybsled: CY2, World) ---------------------------------------------

const RomFile CYBSLED_ROMS[] = {
    { "cy2-mpr-u.3j",    Region::Main,    0, 2, 0x80000 },
    { "cy2-mpr-l.1j",    Region::Main,    1, 2, 0x80000 },
    { "cy2-spr-u.6c",    Region::Slave,   0, 2, 0x80000 },
    { "cy2-spr-l.4c",    Region::Slave,   1, 2, 0x80000 },
    { "cy1-data-u.3a",   Region::Data,    0, 2, 0x80000 },
    { "cy1-data-l.1a",   Region::Data,    1, 2, 0x80000 },
    { "cy1-edata0-u.3b", Region::EData,   0, 2, 0x80000 },
    { "cy1-edata0-l.1b", Region::EData,   1, 2, 0x80000 },
    { "cy1-obj0.5s",     Region::Sprites, 0x000000, 4, 0x80000 },
    { "cy1-obj1.5x",     Region::Sprites, 0x000001, 4, 0x80000 },
    { "cy1-obj2.3s",     Region::Sprites, 0x000002, 4, 0x80000 },
    { "cy1-obj3.3x",     Region::Sprites, 0x000003, 4, 0x80000 },
    { "cy1-obj4.4s",     Region::Sprites, 0x200000, 4, 0x80000 },
    { "cy1-obj5.4x",     Region::Sprites, 0x200001, 4, 0x80000 },
    { "cy1-obj6.2s",     Region::Sprites, 0x200002, 4, 0x80000 },
    { "cy1-obj7.2x",     Region::Sprites, 0x200003, 4, 0x80000 },
    { "cy1-snd0.8j",     Region::Sound,   0, 1, 0x20000 },
    { "cy1-voi0.12b",    Region::Voices,  0x000000, 1, 0x80000 },
    { "cy1-voi1.12c",    Region::Voices,  0x080000, 1, 0x80000 },
    { "cy1-voi2.12d",    Region::Voices,  0x100000, 1, 0x80000 },
    { "cy1-voi3.12e",    Region::Voices,  0x180000, 1, 0x80000 },
    { "cy1-poi-h1.2f",   Region::PointH,  0x00000, 1, 0x80000 },
    { "cy1-poi-lu1.2k",  Region::PointU,  0x00000, 1, 0x80000 },
    { "cy1-poi-ll1.2n",  Region::PointL,  0x00000, 1, 0x80000 },
    { "cy1-poi-h2.2j",   Region::PointH,  0x80000, 1, 0x80000 },
    { "cy1-poi-lu2.2l",  Region::PointU,  0x80000, 1, 0x80000 },
    { "cy1-poi-ll2.2p",  Region::PointL,  0x80000, 1, 0x80000 },
};

// MAME's port H: gun 20h, missile 08h (the dual-port RAM's 10h, as Star
// Blade's second button); its viewport change (02h) is left out.
const GameFunction CYBSLED_FUNCTIONS[] = {
    { "gun",         "GUN",               "p1_button1", 0x20 },
    { "missile",     "MISSILE",           "p1_button2", 0x10 },
    { "left_up",     "LEFT STICK UP",     "p1_up",      0 },
    { "left_down",   "LEFT STICK DOWN",   "p1_down",    0 },
    { "left_left",   "LEFT STICK LEFT",   "p1_left",    0 },
    { "left_right",  "LEFT STICK RIGHT",  "p1_right",   0 },
    { "right_up",    "RIGHT STICK UP",    "p2_up",      0 },
    { "right_down",  "RIGHT STICK DOWN",  "p2_down",    0 },
    { "right_left",  "RIGHT STICK LEFT",  "p2_left",    0 },
    { "right_right", "RIGHT STICK RIGHT", "p2_right",   0 },
};

// Four fonts in tiles 00h-FFh, each 0-9 A-F, its signs, G-Z: the first
// (thin) with a registered sign at 22h, the others with a middle dot (here
// a point) after F; box pieces, the copyright sign and "namco" between them.
const FontRange CYBSLED_FONTS[] = {
    { 0x00, "0123456789ABCDEF" },
    { 0x1f, "." },
    { 0x20, "!\"" },
    { 0x23, "%&'[]*+,-./:;<=>?@GHIJKLMNOPQRSTUVWXYZ" },
    { 0x4d, "'" },
    { 0x4e, "0123456789ABCDEF.!\"%&'[]*+,-./:;<=>?@GHIJKLMNOPQRSTUVWXYZ" },
    { 0x8b, "'" },
    { 0x8c, "0123456789ABCDEF.!\"%&'[]*+,-./:;<=>?@GHIJKLMNOPQRSTUVWXYZ'" },
    { 0xc6, "0123456789ABCDEF.!\"%&'[]*+,-./:;<=>?@GHIJKLMNOPQRSTUVWXYZ'" },
};

// The sled's two sticks (MAME: AN1 / AN3 the left, AN0 / AN2 the right);
// pushed up reads low, as MAME's keys do it.
const Stick CYBSLED_STICKS[] = {
    { 0x0b, 0x09, 0x3f, 0xbf, 0x3f, 0xbf, -1, -1, "p1_stickx", "p1_sticky", false, "left_up", "left_down", "left_left", "left_right", false },
    { 0x0a, 0x08, 0x3f, 0xbf, 0x3f, 0xbf, -1, -1, "p2_stickx", "p2_sticky", false, "right_up", "right_down", "right_left", "right_right", false },
};

const Profile PROFILES[] = {
    {
        "starblad", "Star Blade",
        STARBLAD_ROMS, COUNT(STARBLAD_ROMS), "starblad.nv",
        STARBLAD_FUNCTIONS, COUNT(STARBLAD_FUNCTIONS),
        STARBLAD_STICKS, COUNT(STARBLAD_STICKS),
        { 0xee52, 0xd5df },
        STARBLAD_STREAKS, COUNT(STARBLAD_STREAKS),
        STARBLAD_BACKDROPS, COUNT(STARBLAD_BACKDROPS),
        STARBLAD_FONTS, COUNT(STARBLAD_FONTS),
        STARBLAD_CHARS, COUNT(STARBLAD_CHARS),
        STARBLAD_WORDS, COUNT(STARBLAD_WORDS),
        1.0f,
        -1,                             // text in the game's own colors
        0xf, 120,                       // MAME's bank flip; dark colors kept darker
        STARBLAD_HUD, COUNT(STARBLAD_HUD),
        0,                              // space: no floor
        0, 0, 0,                        // no depth cueing: the planets are seen from far away
    },
    {
        "cybsled", "Cyber Sled",
        CYBSLED_ROMS, COUNT(CYBSLED_ROMS), "cybsled.nv",
        CYBSLED_FUNCTIONS, COUNT(CYBSLED_FUNCTIONS),
        CYBSLED_STICKS, COUNT(CYBSLED_STICKS),
        { 0, 0 },                       // its DSP reports none (captured)
        nullptr, 0, nullptr, 0,         // no models drawn as lines yet
        CYBSLED_FONTS, COUNT(CYBSLED_FONTS),
        nullptr, 0, nullptr, 0,
        0.8f,                           // its letters fill their tiles: drawn smaller
        0xc8c8c8,                       // all text off-white, as the HUD's frames
        0xf, 120,                       // MAME's bank flip; the menus' unselected items stay dark
        nullptr, 0,                     // its HUD: not known yet
        2048,                           // the arena's squares (its pieces' positions step 800h)
        1300, 10500, 0.1f,              // depth cueing: crisp to four sled lengths, 10% from a quarter of the arena on
    },
};

// game.ini's "[system21]" section, its "game = <set>" line; empty if none.
std::string manifestGame()
{
    char path[1024];
    FILE *f = fopen(vp_asset_path("game.ini", path, sizeof path), "r");
    if (!f) return std::string();
    char line[256], set[64];
    bool in = false;
    std::string game;
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') { in = !strncmp(p, "[system21]", 10); continue; }
        if (in && sscanf(p, "game = %63[a-z0-9_]", set) == 1) game = set;
    }
    fclose(f);
    return game;
}

std::string chosenSet;

} // namespace

void profile_choose(const char *set)
{
    chosenSet = set ? set : "";
}

const Profile *profile_find(const char *set)
{
    for (const Profile &p : PROFILES)
        if (set && !strcmp(p.set, set)) return &p;
    return nullptr;
}

const Profile &profile()
{
    static const Profile *chosen = nullptr;
    if (!chosen) {
        const std::string game = chosenSet.empty() ? manifestGame() : chosenSet;
        chosen = profile_find(game.c_str());
        if (!chosen) {
            if (!game.empty()) fprintf(stderr, "system21: %s is not a game known: Star Blade\n", game.c_str());
            chosen = &PROFILES[0];
        }
    }
    return *chosen;
}

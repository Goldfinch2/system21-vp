// profile.h: what is particular to one Namco System 21 (C67) game: its ROM set,
// its controls, the models drawn as lines, its fonts. The machine, the DSP
// board's stand-in, the sprites and the sound board are the same for all; a
// game is a profile. The package says which (game.ini: "[system21]",
// "game = starblad"), Star Blade when it does not.
#pragma once

#include <cstddef>
#include <cstdint>

// Where a ROM file goes (roms.h).
enum class Region { Main, Slave, Data, EData, Sprites, Sound, Voices, PointH, PointU, PointL };

struct RomFile {
    const char *name;       // in the zip
    Region region;
    uint32_t offset;        // its first byte's place in the region
    uint32_t step;          // 1, or 2 / 4 for interleaved halves and quarters
    uint32_t size;          // its size, checked
};

// A function the player lays out (vp_controls.h), and the I/O board's button
// bit it sets (dual-port RAM byte 00h: docs/hardware.md, section 3).
struct GameFunction {
    const char *name;       // vp_input's: "gun"
    const char *label;      // on the CONTROLS page: "GUN"
    const char *standard;   // out of the box: "p1_button1"
    uint8_t bit;
};

// A stick: two of the I/O board's analog channels (dual-port RAM bytes 08h +
// the channel, centered 80h), what moves them.
struct Stick {
    int channelX, channelY;
    uint8_t loX, hiX, loY, hiY;  // each axis's range, all the way over
    const char *axisX, *axisY;  // the advmame.rc axes: "p1_stickx", "p1_sticky"
    bool upHigh;                // pushed up reads high (Star Blade: up aims up)
    // The functions that push it like a stick when held (springing back to
    // the center), or nullptr.
    const char *up, *down, *left, *right;
    bool mouse;                 // the mouse or trackball moves it too
};

// A game that keeps its own stick calibration in its NVRAM, in settings
// records (several, the first marked in use is the one): at every start the
// calibration in each record is set to `x` and `y` (minimum, center, maximum),
// what the stick's range is made for, so a full push is the screen's edge
// whatever the NVRAM held. An NVRAM with no record in use (none in the ROM
// zip) gets one, the ROM's defaults with that calibration.
struct NvCalibration {
    int first, size, count;     // the records: the first's address, each one's size, how many
    uint8_t inUse;              // a record's first byte when it is in use
    int defaults;               // the ROM's default record: its bytes after the mark, as words' low bytes
    int atX, atY;               // where in a record each axis's three bytes are
    uint8_t x[3], y[3];
};

// One of the game's fonts in the sprite ROM: its tiles from `base`, one per
// character of `chars`.
struct FontRange { int base; const char *chars; };
struct FontChar { int tile; char ch; };
struct FontWord { int tile; const char *text; };   // a tile that shows a short word

// Model codes drawn as lines (render3d.cpp, models.cpp), [first, last].
struct CodeRange { int first, last; };

struct Profile {
    const char *set;                    // MAME's set: "starblad" (starblad.zip)
    const char *title;                  // "Star Blade"
    const RomFile *roms; size_t nroms;
    const char *nvram;                  // the factory settings in the zip ("starblad.nv"), or nullptr
    const GameFunction *functions; size_t nfunctions;
    const Stick *sticks; size_t nsticks;
    const NvCalibration *calibration;   // or nullptr
    uint16_t pointSums[2];              // the point ROM checksums the DSP reports at boot, or 0, 0
    const CodeRange *streaks; size_t nstreaks;     // drawn as their long diagonals
    const CodeRange *backdrops; size_t nbackdrops; // pushed out behind everything, their stripes halved
    const FontRange *fonts; size_t nfonts;
    const FontChar *chars; size_t nchars;          // single characters outside the ranges
    const FontWord *words; size_t nwords;
    float textScale;                    // letters' height over their pixels' (Star Blade 1)
    int32_t textRgb;                    // all text in this color (0xRRGGBB), or -1: the game's pens
    // Sprite colors: the bank a sprite's color number picks is flipped by this
    // (MAME's mixer: color ^ Fh); and colors darker than keepDark (their
    // strongest channel) stay that much darker, 0: every color brightened
    // (Cyber Sled dims what is not selected).
    int spriteBankXor, keepDark;
    // The HUD's sprites (their first tile): drawn only at their full size (the
    // game unfolds and squeezes them).
    const CodeRange *hud; size_t nhud;
    // A floor drawn as a grid of squares this size (the arena's, render3d.cpp), or 0.
    int floorCell;
    // Depth cueing (the board darkened polygons by depth): full brightness up
    // to fogStart, fogFloor of it from fogEnd on (world units); fogEnd 0: none.
    float fogStart, fogEnd, fogFloor;
    // The boot self test runs (fast, not shown) until the game has started
    // its DSPs this many times since power-on (Star Blade once, at the end of
    // its test; Cyber Sled twice during it, then again as the game begins,
    // after resetting the DSP board).
    int bootStarts;
    // A game that links two cabinets (docs/link.md): the NVRAM byte whose bit
    // 0 is this cabinet's number, or -1; and its settings' checksum, the sum
    // of bytes 0 to n-1 in byte n (0: none).
    int linkNvram, settingsSum;
};

// The game this package is: from game.ini's [system21] game, Star Blade if
// none. Chosen once, at start.
const Profile &profile();
// Before the first profile(): the game to be, whatever game.ini says
// (--game: running from a build).
void profile_choose(const char *set);
// The profile of a set ("starblad"), or nullptr.
const Profile *profile_find(const char *set);

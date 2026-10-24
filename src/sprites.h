// sprites.h: the C355 sprite layer (score, shield, text, crosshair,
// explosions) as vectors. Text: every tile of the game's fonts is drawn as
// that character in the SDK's Asteroids font, fitted to the original glyph.
// Pictures of text (the briefing's planet labels) are text labels in
// sprites.vec, drawn the same way. The rest from the traces made with the
// sprite editor (sprites.vec): a sprite traced as a whole, or tile by tile.
// A sprite not
// traced yet is not drawn, or, with setShowIds (--sprite-ids), drawn as its
// box with its ID (the first 6 hex digits of its key), to find it in the
// editor. Positions and zoom follow MAME's namco_c355spr.cpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <vectorpie/vp_dvg.h>

class Machine;

class Sprites {
public:
    // tiles: the sprite ROM (16x16 tiles, one byte a pixel), for the glyphs' boxes.
    explicit Sprites(const std::vector<uint8_t> &tiles);
    // sprites.vec; a missing file means nothing is traced.
    void load(const std::string &path);
    void draw(vp_dvg *d, const Machine &machine);
    // The areas of the last draw that hide the 3D (the HUD's panels: H lines
    // in sprites.vec): convex polygons, board units, x y pairs.
    const std::vector<std::vector<float>> &hides() const { return hides_; }
    void setShowIds(bool on) { showIds_ = on; }
    // The sprite list as text, one sprite a line (key, size, place, clip,
    // tiles, how it is drawn): for the pause capture.
    void list(FILE *out, const Machine &machine) const;

private:
    struct Poly {
        uint8_t pen;
        int32_t rgb = -1;           // its own color (#RRGGBB in sprites.vec), or -1: the pen's
        std::vector<float> xy;      // pixels, x y pairs
    };
    struct Trace {
        bool empty = false;         // draw nothing
        std::vector<Poly> polys;
        std::vector<std::vector<float>> hides;  // areas hiding the 3D: pixels, x y pairs, convex
    };

    struct Line {
        float x0, x1, base, height;     // sprite pixels: left, right, baseline, cap height
        std::string text;
    };
    struct Label {
        uint8_t pen = 0;
        int32_t rgb = -1;           // its own color (#RRGGBB), or -1: the pen's
        std::vector<Line> lines;
    };
    struct Glyph {
        char ch = 0;                // 0: not a character tile
        const char *text = nullptr; // a tile showing a short word ("OK"): drawn instead of ch
        uint8_t x0, y0, x1, y1;     // the glyph's box in its tile, pixels
        uint8_t pen;                // its main pixel value (its color)
        uint8_t shades[4];          // its most used pens (a shaded font has several)
        uint8_t nshades = 0;
    };

    // A gauge: a sprite whose first tile is in [lo, hi] is an arc of n
    // segments; each is drawn where the original picture is filled at its
    // middle (the shield). Geometry in sprite pixels: center, y scale of the
    // ellipse, inner and outer radius, from angle a0 to a1 (degrees,
    // anticlockwise, 0 = right).
    struct Gauge {
        int lo, hi, pen, n;
        float cx, cy, yscale, r0, r1, a0, a1;
    };
    std::vector<Gauge> gauges_;
    const std::vector<uint8_t> &tiles_;
    const int tileCount_;              // tile numbers wrap at it, as on the C355 (8192, 16384)

    std::vector<Glyph> glyphs_;                         // by tile number
    std::unordered_map<uint64_t, Trace> spriteTraces_;  // by sprite key
    std::unordered_map<int, Trace> tileTraces_;         // by tile number
    std::unordered_map<int, Label> labels_;             // by the sprite's first tile
    std::unordered_map<uint64_t, Label> keyLabels_;     // by sprite key (two pictures sharing a first tile)
    bool showIds_ = false;
    std::vector<std::vector<float>> hides_;
};

// sprites.cpp: C355 sprites to vectors.
#include "sprites.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vectorpie/vp_text.h>

#include "machine.h"
#include "profile.h"

namespace {

int sext(int v, int bits) { int m = 1 << (bits - 1); v &= (1 << bits) - 1; return (v ^ m) - m; }

// The HUD's pictures: tick and side marks, the shield's frame, the distance
// panel's frame, SCORE, SHIELD, DISTANCE, TO RED EYE (0x35E-0x398); the
// distance's unit and divider. Its digits are the game's digit fonts.
bool isHud(int tile)
{
    const Profile &game = profile();
    for (size_t k = 0; k < game.nhud; k++)
        if (tile >= game.hud[k].first && tile <= game.hud[k].last) return true;
    return false;
}

void brightColor(vp_dvg *d, uint32_t rgb)
{
    int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    int top = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (top < 8) { vp_color(d, 200, 200, 200); return; }
    vp_color(d, (uint8_t)(r * 255 / top), (uint8_t)(g * 255 / top), (uint8_t)(b * 255 / top));
}

// How bright a color is drawn when the game keeps dark colors dark
// (profile.h keepDark): 1 from keepDark up, less below it, at least DARK_MIN.
const float DARK_MIN = 0.15f;
float darkness(uint32_t rgb)
{
    const int keep = profile().keepDark;
    if (keep <= 0) return 1.0f;
    int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    int top = r > g ? (r > b ? r : b) : (g > b ? g : b);
    return top >= keep ? 1.0f : std::max(DARK_MIN, (float)top / keep);
}

void scaledColor(vp_dvg *d, uint32_t rgb, float k)
{
    vp_color(d, (uint8_t)((rgb >> 16 & 0xff) * k), (uint8_t)((rgb >> 8 & 0xff) * k), (uint8_t)((rgb & 0xff) * k));
}

// A sprite image's key: its size, tile offset and tile list (the sprite
// editor's catalog computed the same).
uint64_t spriteKey(const uint16_t *tiles, int cols, int rows, int offset)
{
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix((uint64_t)cols << 8 | rows);
    mix((uint64_t)(offset & 0xffff));
    for (int i = 0; i < cols * rows; i++) mix(tiles[i]);
    return key;
}

// Copies of a sprite this near (pixels) are its shadow or outline (Cyber
// Sled: 4 off; its radar blips, 8 apart, are not).
const int COPY_NEAR = 4;

} // namespace

// The game's fonts in the sprite ROM (profile.h; Star Blade: the ASCII font
// at 20h-7Fh, five more fonts as A-Z, 0-9 and their symbols, three digit sets,
// and some extras).
Sprites::Sprites(const std::vector<uint8_t> &tiles) : tiles_(tiles), tileCount_((int)(tiles.size() / 256))
{
    const int count = (int)(tiles.size() / 256);
    glyphs_.assign(count, Glyph());
    auto setChar = [&](int t, char ch) {
        if (t < 0 || t >= count) return;
        const uint8_t *px = &tiles[(size_t)t * 256];
        int x0 = 16, y0 = 16, x1 = -1, y1 = -1;
        unsigned counts[256] = {};
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t p = px[y * 16 + x];
                if (p == 0xff || p == 0x00 || p == 0x01) continue;
                counts[p]++;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        Glyph &g = glyphs_[t];
        g.ch = ch;
        if (x1 < 0) { g.ch = ' '; return; }         // blank: a space
        g.x0 = (uint8_t)x0; g.y0 = (uint8_t)y0; g.x1 = (uint8_t)(x1 + 1); g.y1 = (uint8_t)(y1 + 1);
        unsigned best = 0;
        for (int p = 0; p < 256; p++)
            if (counts[p] > best) { best = counts[p]; g.pen = (uint8_t)p; }
        // Its four most used pens: how bright the letter is drawn goes by the
        // brightest of them (a shaded letter has dark pens too).
        g.nshades = 0;
        for (int n = 0; n < 4; n++) {
            int top = -1;
            for (int p = 0; p < 256; p++)
                if (counts[p] && (top < 0 || counts[p] > counts[top])) top = p;
            if (top < 0) break;
            g.shades[g.nshades++] = (uint8_t)top;
            counts[top] = 0;
        }
    };
    const Profile &game = profile();
    for (size_t k = 0; k < game.nfonts; k++)
        for (int i = 0; game.fonts[k].chars[i]; i++) setChar(game.fonts[k].base + i, game.fonts[k].chars[i]);
    for (size_t k = 0; k < game.nchars; k++) setChar(game.chars[k].tile, game.chars[k].ch);
    // Tiles showing a short word.
    for (size_t k = 0; k < game.nwords; k++) {
        const FontWord &w = game.words[k];
        setChar(w.tile, '?');
        if (w.tile < count && glyphs_[w.tile].ch != ' ') glyphs_[w.tile].text = w.text;
    }
}

// sprites.vec: "S <key hex>" or "T <tile hex>", then "E" (draw nothing) or
// "P <pen> <count> x y ..." lines (the pen may be a color: #RRGGBB); "X <first tile hex, or sprite key> <pen or #RRGGBB>", then
// "L <x0> <x1> <baseline> <height> <text>" lines: a picture of text (text
// starting with "@<pen> ": that line's own pen; then ">": right-aligned;
// "--": a dash, a straight line across the box; "[]": a rectangle around it);
// "G <lo hex> <hi hex> <pen> <segments> <cx> <cy> <yscale> <r0> <r1> <a0> <a1>": a gauge;
// after S or T, "H x0 y0 x1 y1" (a rectangle) or "H x y x y x y ..." (a convex
// polygon): an area of the picture that hides the 3D (a HUD panel);
// "O <first tile hex, or sprite key> <x> <y>", then L lines: text of our own,
// in screen pixels (496 x 480), while that sprite is drawn at that place.
void Sprites::load(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    Trace *cur = nullptr;
    Label *label = nullptr;
    int overlay = -1;                   // the overlay L lines go to, or -1
    char line[65536];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 'X') {
            // a first tile (4 hex digits) or a whole sprite's key (16)
            char *start = p + 1, *end;
            while (*start == ' ') start++;
            unsigned long long id = strtoull(start, &end, 16);
            overlay = -1;
            label = (end - start > 8) ? &keyLabels_[(uint64_t)id] : &labels_[(int)id];
            *label = Label();
            while (*end == ' ' || *end == '\t') end++;
            if (*end == '#') label->rgb = (int32_t)strtol(end + 1, nullptr, 16);
            else label->pen = (uint8_t)strtol(end, nullptr, 10);
            cur = nullptr;
            continue;
        }
        if (*p == 'O') {
            char *start = p + 1, *end;
            while (*start == ' ') start++;
            Overlay o;
            const unsigned long long id = strtoull(start, &end, 16);
            if (end - start > 8) o.key = (uint64_t)id; else o.first = (int)id;
            o.x = (int)strtol(end, &end, 10);
            o.y = (int)strtol(end, &end, 10);
            overlays_.push_back(o);
            overlay = (int)overlays_.size() - 1;
            label = nullptr;
            cur = nullptr;
            continue;
        }
        if (*p == 'L' && (label || overlay >= 0)) {
            Line l;
            char *end = p + 1;
            l.x0 = strtof(end, &end);
            l.x1 = strtof(end, &end);
            l.base = strtof(end, &end);
            l.height = strtof(end, &end);
            while (*end == ' ') end++;
            l.text = end;
            while (!l.text.empty() && (l.text.back() == '\n' || l.text.back() == '\r')) l.text.pop_back();
            if (label) label->lines.push_back(l);
            else overlays_[(size_t)overlay].lines.push_back(l);
            continue;
        }
        if (*p == 'G' || *p == 'S' || *p == 'T') overlay = -1;
        if (*p == 'G') {
            Gauge g;
            char *end = p + 1;
            g.lo = (int)strtol(end, &end, 16);
            g.hi = (int)strtol(end, &end, 16);
            g.pen = (int)strtol(end, &end, 10);
            g.n = (int)strtol(end, &end, 10);
            g.cx = strtof(end, &end); g.cy = strtof(end, &end); g.yscale = strtof(end, &end);
            g.r0 = strtof(end, &end); g.r1 = strtof(end, &end);
            g.a0 = strtof(end, &end); g.a1 = strtof(end, &end);
            if (g.n > 0) gauges_.push_back(g);
            cur = nullptr;
            label = nullptr;
            continue;
        }
        if (*p == 'S' || *p == 'T') {
            label = nullptr;
            unsigned long long id = strtoull(p + 1, nullptr, 16);
            cur = (*p == 'S') ? &spriteTraces_[(uint64_t)id] : &tileTraces_[(int)id];
            *cur = Trace();
        } else if (*p == 'E' && cur) {
            cur->empty = true;
        } else if (*p == 'H' && cur) {
            std::vector<float> v;
            char *end = p + 1, *next;
            for (float f = strtof(end, &next); next != end; f = strtof(end, &next)) {
                v.push_back(f);
                end = next;
            }
            if (v.size() == 4) v = { v[0], v[1], v[2], v[1], v[2], v[3], v[0], v[3] };
            if (v.size() >= 6 && v.size() % 2 == 0) cur->hides.push_back(v);
        } else if (*p == 'P' && cur) {
            char *end;
            Poly poly;
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '#') {
                poly.rgb = (int32_t)strtol(q + 1, &end, 16);
                poly.pen = 0;
            } else {
                poly.pen = (uint8_t)strtol(q, &end, 10);
            }
            long n = strtol(end, &end, 10);
            for (long i = 0; i < 2 * n; i++) {
                char *next;
                float v = strtof(end, &next);
                if (next == end) break;
                poly.xy.push_back(v);
                end = next;
            }
            if (poly.xy.size() >= 4) cur->polys.push_back(std::move(poly));
        }
    }
    fclose(f);
}

void Sprites::draw(vp_dvg *d, const Machine &m)
{
    hides_.clear();
    const uint16_t *ram = m.spriteram();
    const uint16_t *pos = m.spritePosition();
    const uint16_t *table = ram;                    // 0x0000: 8 words per sprite
    const uint16_t *list = ram + 0x2000 / 2;
    const uint16_t *clipTable = ram + 0x2400 / 2;
    const uint16_t *format = ram + 0x4000 / 2;
    const uint16_t *tileRam = ram + 0x8000 / 2;

    // Medium resolution (System 21): 10-bit X scroll; the driver's offsets (0, 20h).
    const int xscroll = sext(pos[1], 10);
    const int yscroll = sext(pos[0], 9) + 0x20;
    const float sx = 4096.0f / 496.0f, sy = 4096.0f / 480.0f;
    vp_text_font(VP_FONT_ASTEROIDS);

    // A sprite drawn again a little apart, the same picture at the same
    // size, later in the list, is a shadow or an outline of that one
    // (Cyber Sled's titles: four dark copies 4 pixels off, then the white
    // one in the middle): only the last is drawn.
    struct Copy { uint64_t key; int x, y, w, h; };
    Copy copies[256];
    int count = 0;
    std::vector<bool> overlaid(overlays_.size(), false);    // the overlays to show this frame
    for (int i = 0; i < 256; i++) {
        const uint16_t which = list[i];
        const uint16_t *e = table + ((which & 0xff) << 3);
        const uint16_t *f = format + ((e[0] & 0x7ff) << 2);
        int cols = (f[1] >> 4) & 0xf, rows = f[1] & 0xf;
        if (!cols) cols = 16;
        if (!rows) rows = 16;
        Copy &c = copies[count++];
        c.key = f[0] + cols * rows <= 0x8000 / 2 ? spriteKey(tileRam + f[0], cols, rows, e[1]) : 0;
        c.x = e[2]; c.y = e[3]; c.w = e[4]; c.h = e[5];
        if (which & 0x100) break;
    }
    auto shadow = [&](int i) {
        const Copy &a = copies[i];
        for (int j = i + 1; j < count; j++) {
            const Copy &b = copies[j];
            if (b.key == a.key && b.w == a.w && b.h == a.h &&
                std::abs((int16_t)(b.x - a.x)) <= COPY_NEAR && std::abs((int16_t)(b.y - a.y)) <= COPY_NEAR)
                return true;
        }
        return false;
    };

    for (int i = 0; i < 256; i++) {
        uint16_t which = list[i];
        if (i < count && copies[i].key && shadow(i)) {
            if (which & 0x100) break;
            continue;
        }
        const uint16_t *e = table + ((which & 0xff) << 3);
        uint16_t palette = e[6];
        int link = e[0] & 0x7ff;
        int offset = e[1];
        int hpos = sext(e[2] - xscroll, 11);
        int vpos = sext(e[3] - yscroll, 11);
        const int placeX = hpos, placeY = vpos;     // where it is, before its offsets (the overlays')
        int hsize = e[4], vsize = e[5];
        const uint16_t *f = format + (link << 2);
        int tileIndex = f[0];
        int cols = (f[1] >> 4) & 0xf, rows = f[1] & 0xf;
        int dx = f[2] & 0x1ff, dy = f[3] & 0x1ff;
        if (!cols) cols = 16;
        if (!rows) rows = 16;
        bool flipx = hsize & 0x8000, flipy = vsize & 0x8000;
        hsize &= 0x3ff;
        vsize &= 0x3ff;

        if (hsize && vsize && tileIndex + cols * rows <= 0x8000 / 2) {
            const uint16_t *tiles = tileRam + tileIndex;
            const int W = cols * 16, H = rows * 16;
            float zx = hsize / (float)W, zy = vsize / (float)H;
            int dxz = (int)((dx & 0xff) * zx + 0.5f), dyz = (int)((dy & 0xff) * zy + 0.5f);
            if (dx & 0x100) dxz = -dxz;
            if (dy & 0x100) dyz = -dyz;
            hpos += flipx ? dxz : -dxz;
            vpos += flipy ? dyz : -dyz;
            // The picture: [hpos, hpos + hsize), or [hpos - hsize, hpos) mirrored.
            const float x0 = (float)(flipx ? hpos - hsize : hpos);
            const float y0 = (float)(flipy ? vpos - vsize : vpos);
            auto bx = [&](float px) { return (int)((x0 + (flipx ? W - px : px) * zx) * sx); };
            auto by = [&](float py) { return (int)(4095 - (y0 + (flipy ? H - py : py) * zy) * sy); };
            // A pen's color: from the bank the sprite's color number picks (flipped
            // as on the board: profile.h), brightened, dark ones kept darker.
            auto penRgb = [&](int pen) { return m.paletteRgb(0x1000 + (((palette ^ profile().spriteBankXor) & 0xf) << 8) + pen); };
            auto penColor = [&](int pen) {
                const uint32_t rgb = penRgb(pen);
                const float k = darkness(rgb);
                if (k >= 1.0f) { brightColor(d, rgb); return; }
                int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
                int top = std::max(r, std::max(g, b));
                if (top < 1) top = 1;
                scaledColor(d, (uint32_t)(r * 255 / top) << 16 | (uint32_t)(g * 255 / top) << 8 | (uint32_t)(b * 255 / top), k);
            };
            // Text: the game's one text color (profile.h), as dark as its pen is; else its pen's.
            auto textColor = [&](int pen) {
                const int32_t rgb = profile().textRgb;
                if (rgb >= 0) scaledColor(d, (uint32_t)rgb, darkness(penRgb(pen)));
                else penColor(pen);
            };
            // A font letter: as bright as the brightest of its shades (a dim
            // letter, not selected, is dark in all of them).
            auto glyphColor = [&](const Glyph &g) {
                const int32_t rgb = profile().textRgb;
                if (rgb < 0 || !g.nshades) { textColor(g.pen); return; }
                float k = 0;
                for (int n = 0; n < g.nshades; n++) k = std::max(k, darkness(penRgb(g.shades[n])));
                scaledColor(d, (uint32_t)rgb, k);
            };
            auto drawTrace = [&](const Trace &t, float ox, float oy) {
                for (const std::vector<float> &h : t.hides) {
                    std::vector<float> area;
                    for (size_t k = 0; k + 1 < h.size(); k += 2) {
                        area.push_back((float)bx(ox + h[k]));
                        area.push_back((float)by(oy + h[k + 1]));
                    }
                    hides_.push_back(std::move(area));
                }
                for (const Poly &p : t.polys) {
                    if (p.rgb >= 0) vp_color(d, (uint8_t)(p.rgb >> 16), (uint8_t)(p.rgb >> 8), (uint8_t)p.rgb);
                    else penColor(p.pen);
                    vp_moveto(d, bx(ox + p.xy[0]), by(oy + p.xy[1]));
                    for (size_t k = 2; k + 1 < p.xy.size(); k += 2) vp_lineto(d, bx(ox + p.xy[k]), by(oy + p.xy[k + 1]));
                }
            };

            int ci = (palette >> 8) & 0xf;
            // Its clip window (pixels, inclusive). An empty one, or one off
            // the picture, hides the sprite: the game hides sprites that way
            // (a blinking marker), and MAME draws nothing.
            const uint16_t *c = clipTable + (ci << 2);
            int px0 = c[0] - xscroll, px1 = c[1] - xscroll, py0 = c[2] - yscroll, py1 = c[3] - yscroll;
            px0 = std::max(px0, 0); py0 = std::max(py0, 0);
            px1 = std::min(px1, 495); py1 = std::min(py1, 479);
            if (px1 < px0 || py1 < py0) {
                if (which & 0x100) break;
                continue;
            }
            auto clampc = [](int v) { return v < 0 ? 0 : (v > 4095 ? 4095 : v); };
            vp_dvg_set_clip(d, clampc((int)(px0 * sx)), clampc((int)(4095 - (py1 + 1) * sy)),
                            clampc((int)((px1 + 1) * sx) - 1), clampc((int)(4095 - py0 * sy)));

            uint64_t key = spriteKey(tiles, cols, rows, offset);
            auto st = spriteTraces_.find(key);
            int first = -1;
            for (int t = 0; t < cols * rows && first < 0; t++)
                if (!(tiles[t] & 0x8000)) first = (tiles[t] + offset) % tileCount_;
            for (size_t o = 0; o < overlays_.size(); o++) {
                const Overlay &ov = overlays_[o];
                if ((ov.key ? ov.key == key : ov.first == first) && ov.x == placeX && ov.y == placeY)
                    overlaid[o] = true;
            }
            // The HUD (its marks, frames, labels and the distance): the game
            // unfolds it from a line at the start and squeezes it into the
            // middle at the game over. Drawn only at its full size: it shows
            // up and goes away at once.
            if (isHud(first) && (zx < 1.0f || zy < 1.0f)) {
                vp_dvg_reset_clip(d);
                if (which & 0x100) break;
                continue;
            }
            const Gauge *gauge = nullptr;
            for (const Gauge &g : gauges_)
                if (first >= g.lo && first <= g.hi) gauge = &g;
            if (gauge) {
                // The original picture's pixel at (x, y), sprite pixels.
                auto opaqueAt = [&](float fx, float fy) {
                    int x = (int)fx, y = (int)fy;
                    if (x < 0 || y < 0 || x >= W || y >= H) return false;
                    uint16_t tw = tiles[(y / 16) * cols + x / 16];
                    if (tw & 0x8000) return false;
                    size_t at = (size_t)((tw + offset) % tileCount_) * 256 + (y % 16) * 16 + x % 16;
                    if (at >= tiles_.size()) return false;
                    uint8_t p = tiles_[at];
                    return p != 0xff && p != 0x00 && p != 0x01;
                };
                const float rad = 3.14159265f / 180.0f;
                auto pt = [&](float r, float a, int &ox, int &oy) {
                    ox = bx(gauge->cx + r * std::cos(a * rad));
                    oy = by(gauge->cy - r * gauge->yscale * std::sin(a * rad));
                };
                penColor(gauge->pen);
                const float step = (gauge->a1 - gauge->a0) / gauge->n;
                for (int k = 0; k < gauge->n; k++) {
                    float a0 = gauge->a0 + step * k, a1 = a0 + step, am = a0 + step * 0.5f;
                    float rm = (gauge->r0 + gauge->r1) * 0.5f;
                    if (!opaqueAt(gauge->cx + rm * std::cos(am * rad), gauge->cy - rm * gauge->yscale * std::sin(am * rad)))
                        continue;
                    int qx[4], qy[4];
                    pt(gauge->r0, a0, qx[0], qy[0]); pt(gauge->r1, a0, qx[1], qy[1]);
                    pt(gauge->r1, a1, qx[2], qy[2]); pt(gauge->r0, a1, qx[3], qy[3]);
                    vp_moveto(d, qx[0], qy[0]);
                    for (int q = 1; q <= 4; q++) vp_lineto(d, qx[q % 4], qy[q % 4]);
                }
                vp_dvg_reset_clip(d);
                if (which & 0x100) break;
                continue;
            }
            const Label *label = nullptr;
            auto kl = keyLabels_.find(key);
            if (kl != keyLabels_.end()) label = &kl->second;
            else if (first >= 0) {
                auto lb = labels_.find(first);
                if (lb != labels_.end()) label = &lb->second;
            }
            // A drawing and a label can go together (a frame with words in it).
            if (st != spriteTraces_.end()) drawTrace(st->second, 0, 0);
            if (st != spriteTraces_.end() && !label) {
                // drawn
            } else if (label) {
                // A picture of text: its lines, in the Asteroids font, and
                // whatever of it is drawn in its tiles (a frame around words).
                for (int t = 0; t < cols * rows; t++) {
                    if (tiles[t] & 0x8000) continue;
                    auto tt = tileTraces_.find((tiles[t] + offset) % tileCount_);
                    if (tt != tileTraces_.end())
                        drawTrace(tt->second, (float)(t % cols * 16), (float)(t / cols * 16));
                }
                for (const Line &l : label->lines) {
                    // "@pen text": its own pen; ">text": right-aligned (a column of values)
                    const char *text = l.text.c_str();
                    int pen = label->pen;
                    bool own = label->rgb >= 0;     // the label's own color, unless the line names a pen
                    if (*text == '@') {
                        char *end;
                        pen = (int)strtol(text + 1, &end, 10);
                        own = false;
                        text = end;
                        while (*text == ' ') text++;
                    }
                    const bool right = *text == '>';
                    if (right) text++;
                    if (own) vp_color(d, (uint8_t)(label->rgb >> 16), (uint8_t)(label->rgb >> 8), (uint8_t)label->rgb);
                    else textColor(pen);
                    if (!strcmp(text, "[]")) {          // a frame: a rectangle around the box
                        int x0 = bx(l.x0), x1 = bx(l.x1), y0 = by(l.base - l.height), y1 = by(l.base);
                        vp_moveto(d, x0, y0);
                        vp_lineto(d, x1, y0);
                        vp_lineto(d, x1, y1);
                        vp_lineto(d, x0, y1);
                        vp_lineto(d, x0, y0);
                        continue;
                    }
                    if (!strcmp(text, "--")) {          // a dash: a line across the box, mid-height
                        int ya = by(l.base - l.height * 0.5f);
                        vp_moveto(d, bx(l.x0), ya);
                        vp_lineto(d, bx(l.x1), ya);
                        continue;
                    }
                    float xa = (float)bx(l.x0), xb = (float)bx(l.x1);
                    float base = (float)by(l.base), top = (float)by(l.base - l.height);
                    // The original's height; a line wider than the original's
                    // gets narrower letters, not smaller ones.
                    float height = std::fabs(top - base);
                    float room = std::fabs(xb - xa);
                    vp_text_fit(d, text, right ? std::max(xa, xb) : std::min(xa, xb), std::min(base, top), height,
                                room, right ? VP_TEXT_RIGHT : VP_TEXT_LEFT);
                }
            } else {
                // Tile by tile: characters in the Asteroids font, the rest
                // from tile traces; any tile without either: not done.
                bool missing = false;
                for (int t = 0; t < cols * rows; t++) {
                    if (tiles[t] & 0x8000) continue;
                    const int tile = (tiles[t] + offset) % tileCount_;
                    const float tx = (float)(t % cols * 16), ty = (float)(t / cols * 16);
                    if (tile < (int)glyphs_.size() && glyphs_[tile].ch) {
                        const Glyph &g = glyphs_[tile];
                        if (g.ch == ' ') continue;
                        // The glyph's box on the board (a mirrored sprite: still upright text).
                        float xa = (float)bx(tx + g.x0), xb = (float)bx(tx + g.x1);
                        float ya = (float)by(ty + g.y0), yb = (float)by(ty + g.y1);
                        // The game's letter size (profile.h), centered on the box.
                        float height = std::fabs(ya - yb);
                        if (height < 1) continue;
                        const float scaled = height * profile().textScale;
                        if (ya < yb) { ya += (height - scaled) * 0.5f; yb = ya + scaled; }
                        else { yb += (height - scaled) * 0.5f; ya = yb + scaled; }
                        height = scaled;
                        const char str[2] = { g.ch, 0 };
                        glyphColor(g);
                        if (g.text)
                            vp_text_fit(d, g.text, (xa + xb) * 0.5f, std::min(ya, yb), height, std::fabs(xb - xa),
                                        VP_TEXT_CENTER);
                        else if (g.ch == '+' || g.ch == '-') {
                            // A sign covers only the middle of a letter's
                            // height: sized by its width (the Asteroids font
                            // draws it 6 units wide of a 12-unit capital),
                            // centered on its box.
                            const float h = std::fabs(xb - xa) * 2.0f;
                            vp_text(d, str, (xa + xb) * 0.5f, (ya + yb) * 0.5f - h * 0.5f, h, VP_TEXT_CENTER);
                        } else {
                            // At most its tile's width on the screen (a squeezed
                            // sprite: narrower letters, never overlapping).
                            const float cell = std::fabs((float)(bx(tx + 15) - bx(tx + 1)));
                            vp_text_fit(d, str, (xa + xb) * 0.5f, std::min(ya, yb), height, cell, VP_TEXT_CENTER);
                        }
                        continue;
                    }
                    auto tt = tileTraces_.find(tile);
                    if (tt == tileTraces_.end()) { missing = true; continue; }
                    drawTrace(tt->second, tx, ty);
                }
                if (missing && showIds_) {
                    // Its box and its ID, in the sprite's bank color.
                    int ax = bx(0), ay = by(0), bx1 = bx((float)W), by1 = by((float)H);
                    vp_color(d, 0, 160, 255);
                    vp_moveto(d, ax, ay);
                    vp_lineto(d, bx1, ay);
                    vp_lineto(d, bx1, by1);
                    vp_lineto(d, ax, by1);
                    vp_lineto(d, ax, ay);
                    char id[8];
                    snprintf(id, sizeof id, "%06X", (unsigned)(key >> 40));
                    float boxW = (float)abs(bx1 - ax), boxH = (float)abs(by1 - ay);
                    float hgt = boxH * 0.5f;
                    float wid = vp_text_width(id, hgt);
                    if (wid > boxW * 0.9f) hgt *= boxW * 0.9f / wid;
                    if (hgt < 30) hgt = 30;
                    vp_color(d, 255, 255, 255);
                    vp_text(d, id, (ax + bx1) * 0.5f, (ay < by1 ? ay : by1) + boxH * 0.5f - hgt * 0.5f, hgt, VP_TEXT_CENTER);
                }
            }
            vp_dvg_reset_clip(d);
        }
        if (which & 0x100) break;
    }
    // Our own text, over the screen it goes with: centered in its box, in the
    // game's text color.
    vp_dvg_reset_clip(d);
    const int32_t rgb = profile().textRgb >= 0 ? profile().textRgb : 0xffffff;
    for (size_t o = 0; o < overlays_.size(); o++) {
        if (!overlaid[o]) continue;
        vp_color(d, (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
        for (const Line &l : overlays_[o].lines) {
            const float base = 4095 - l.base * sy, height = l.height * sy;
            vp_text_fit(d, l.text.c_str(), (l.x0 + l.x1) * 0.5f * sx, base, height, (l.x1 - l.x0) * sx, VP_TEXT_CENTER);
        }
    }
    vp_text_font(VP_FONT_HERSHEY);
}

void Sprites::list(FILE *out, const Machine &m) const
{
    const uint16_t *ram = m.spriteram();
    const uint16_t *pos = m.spritePosition();
    const uint16_t *table = ram;
    const uint16_t *list = ram + 0x2000 / 2;
    const uint16_t *clipTable = ram + 0x2400 / 2;
    const uint16_t *format = ram + 0x4000 / 2;
    const uint16_t *tileRam = ram + 0x8000 / 2;
    const int xscroll = sext(pos[1], 10);
    const int yscroll = sext(pos[0], 9) + 0x20;

    for (int i = 0; i < 256; i++) {
        uint16_t which = list[i];
        const uint16_t *e = table + ((which & 0xff) << 3);
        uint16_t palette = e[6];
        int link = e[0] & 0x7ff;
        int offset = e[1];
        const uint16_t *f = format + (link << 2);
        int tileIndex = f[0];
        int cols = (f[1] >> 4) & 0xf, rows = f[1] & 0xf;
        if (!cols) cols = 16;
        if (!rows) rows = 16;
        int hsize = e[4] & 0x3ff, vsize = e[5] & 0x3ff;
        if (hsize && vsize && tileIndex + cols * rows <= 0x8000 / 2) {
            const uint16_t *tiles = tileRam + tileIndex;
            uint64_t key = spriteKey(tiles, cols, rows, offset);
            int first = -1;
            for (int t = 0; t < cols * rows && first < 0; t++)
                if (!(tiles[t] & 0x8000)) first = (tiles[t] + offset) % tileCount_;
            const uint16_t *c = clipTable + (((palette >> 8) & 0xf) << 2);
            const char *how = "tiles";
            if (spriteTraces_.count(key)) how = "sprite trace";
            for (const Gauge &g : gauges_)
                if (first >= g.lo && first <= g.hi) how = "gauge";
            if (keyLabels_.count(key) || labels_.count(first)) how = "label";
            fprintf(out, "%3d %016llx %d %d %04x pal %04x x=%d y=%d w=%d h=%d flip=%d%d dxy=%03x,%03x clip=%d-%d,%d-%d %s tiles",
                    i, (unsigned long long)key, cols, rows, offset & 0xffff, palette,
                    sext(e[2] - xscroll, 11), sext(e[3] - yscroll, 11), hsize, vsize,
                    e[4] >> 15, e[5] >> 15, f[2] & 0x1ff, f[3] & 0x1ff,
                    c[0] - xscroll, c[1] - xscroll, c[2] - yscroll, c[3] - yscroll, how);
            for (int t = 0; t < cols * rows; t++) fprintf(out, " %04x", tiles[t]);
            fprintf(out, "\n");
        }
        if (which & 0x100) break;
    }
}

// render3d.cpp: the master and slave DSPs' work (docs/dsp.md), done in float:
// object matrix, camera matrix, projection; the models drawn as outlines in
// an SDK scene, hidden lines removed, instead of filled polygons.
#include "render3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "machine.h"
#include "models.h"
#include "profile.h"

namespace {

struct Mat { float m[9]; };

Mat mul(const Mat &a, const Mat &b)
{
    Mat r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}

Mat rotX(float s, float c) { return {{ 1, 0, 0,  0, c, -s,  0, s, c }}; }
Mat rotY(float s, float c) { return {{ c, 0, s,  0, 1, 0,  -s, 0, c }}; }
Mat rotZ(float s, float c) { return {{ c, -s, 0,  s, c, 0,  0, 0, 1 }}; }

int16_t s16(uint16_t v) { return (int16_t)v; }

// The six rotation orders of the master DSP (routines 892Bh..8A66h):
// angle 1 turns about X, angle 2 about Y, angle 3 about Z.
// `w` points at s1, c1, s2, c2, s3, c3 (Q15).
Mat rotation(const uint16_t *w, int order)
{
    const float k = 1.0f / 32768.0f;
    Mat x = rotX(s16(w[0]) * k, s16(w[1]) * k);
    Mat y = rotY(s16(w[2]) * k, s16(w[3]) * k);
    Mat z = rotZ(s16(w[4]) * k, s16(w[5]) * k);
    switch (order) {
    default:
    case 0: return mul(mul(z, y), x);
    case 1: return mul(mul(y, z), x);
    case 2: return mul(mul(z, x), y);
    case 3: return mul(mul(x, z), y);
    case 4: return mul(mul(y, x), z);
    case 5: return mul(mul(x, y), z);
    }
}

vp_vec3 apply(const Mat &a, vp_vec3 p)
{
    return vp_v3(a.m[0] * p.x + a.m[1] * p.y + a.m[2] * p.z,
                 a.m[3] * p.x + a.m[4] * p.y + a.m[5] * p.z,
                 a.m[6] * p.x + a.m[7] * p.y + a.m[8] * p.z);
}

// A color made as bright as it can be (the hue kept): on a vector monitor a
// dark raster shade is a faint line. Black: a light blue.
void brightColor(uint32_t rgb, uint8_t *out)
{
    int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
    int top = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (top < 8) { out[0] = 120; out[1] = 160; out[2] = 255; return; }
    out[0] = (uint8_t)(r * 255 / top);
    out[1] = (uint8_t)(g * 255 / top);
    out[2] = (uint8_t)(b * 255 / top);
}

// The horizon band: model 2 (also 3448), a strip the game keeps 16 to 3300 units in
// front of the camera, while the ships and planets it must stay behind are
// farther (the board sorted it last with a depth bias of 15000). A backdrop:
// pushed out along the lines of sight by BACKDROP_SCALE, it shows on the
// screen where it did, behind everything.
const float BACKDROP_SCALE = 4096.0f;

// The warp: up to 421 streaks per frame (sb_isStreak): more than a vector
// monitor redraws without flicker. Only the nearest MAX_STREAKS are drawn:
// the long, fast ones; the far ones are specks at the center.
const int MAX_STREAKS = 15;

// The floor (Cyber Sled): the ground the sled stands on, as a grid of the
// arena's own squares, FLOOR_CELLS of them each way from the camera. The
// sled: the objects nearer the camera than SLED_NEAR.
const int FLOOR_CELLS = 10;
const float SLED_NEAR = 600.0f;
const float FLOOR_LIFT = 5.0f;          // over the ground's faces and the markings on them
const uint8_t FLOOR_RGB[3] = { 20, 26, 40 };   // the floor grid: dim, under everything
const float FLOOR_DIM = 0.38f;          // the drawings on the floor: their colors this bright
const float FLAT = 2.0f;                // a model this thin, or less: drawn flat on the floor
const bool FLOOR_DRAWINGS = false;      // the drawings on the floor (stripes, triangles): off, the grid only
const float SMALL_SQUARE = 160.0f;      // a flat square this wide or less: one of the floor's small squares, left out
const uint16_t SHADOW_COLOR = 0x2ba;    // the sleds' shadows' color word (Cyber Sled): kept, small as they are
const float HUD_Z = 20.0f;              // the HUD's screens: just past the near plane (16)

// An object record's length in words, or 0 (the end, or a type not known).
size_t recordLength(uint16_t type)
{
    return (type == 1 || type == 2) ? 13 : type == 0 ? 6 : type == 0x100 ? 11 : type == 0x101 ? 14 : 0;
}

} // namespace

Render3d::Render3d() = default;

Render3d::~Render3d()
{
    for (auto &m : models_) vp3d_mesh_free(m.second.mesh);
    vp3d_scene_free(scene_);
}

int Render3d::open(const std::vector<int32_t> &points)
{
    points_ = &points;
    return vp3d_scene_create(&scene_);
}

// A model, built on first use from the point ROM (models.cpp). Every
// polygon edge drawn (crease 0), and every face hides from both sides: many
// of the models are open shapes (rings, plates, the tunnel). A face's color
// word picks palette entry 3E00h + its low byte, in the upper 256 of that
// block unless bit 200h is set.
Render3d::Model &Render3d::model(int code)
{
    auto it = models_.find(code);
    if (it != models_.end()) return it->second;
    Model &m = models_[code];
    SbModel rom;
    if (!points_ || !sb_model(*points_, code, rom)) return m;
    m.verts = rom.verts;
    for (const SbModel::Line &l : rom.lines) { m.verts.push_back(l.a); m.verts.push_back(l.b); }
    for (size_t k = 0; k < m.verts.size(); k++)
        if (k == 0 || m.verts[k].y < m.minY) m.minY = m.verts[k].y;
    auto entryOf = [](int word) { return (uint16_t)((word & 0xff) | (word & 0x200 ? 0 : 0x100)); };
    // A model of lines only (the warp's streaks, the horizon band): drawn
    // line by line, each in its own color (the SDK's lines have none).
    if (!rom.lines.empty()) {
        for (const SbModel::Line &l : rom.lines) m.lines.push_back({ l.a, l.b, entryOf(l.color) });
        return m;
    }
    // A flat model where the game has a floor (Cyber Sled: the ground, the
    // drawings on it, the sled's shadow, each a few units over the other):
    // its faces' outlines, hiding nothing. As solids they would cut each
    // other's edges, and the floor's grid.
    if (profile().floorCell > 0 && !rom.faces.empty() && rom.verts.size() > 2) {
        float lo = rom.verts[0].y, hi = lo;
        for (const vp_vec3 &v : rom.verts) { lo = std::min(lo, v.y); hi = std::max(hi, v.y); }
        if (hi - lo <= FLAT) {
            std::set<std::pair<int, int>> seen;
            for (const SbModel::Face &f : rom.faces) {
                // The floor's small squares (64 or 128 wide, dotted along the
                // stripes): left out, for a simpler floor. Not the sleds'
                // shadows, small too (their color: SHADOW_COLOR).
                float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
                for (int c : f.corners) {
                    x0 = std::min(x0, rom.verts[c].x); x1 = std::max(x1, rom.verts[c].x);
                    z0 = std::min(z0, rom.verts[c].z); z1 = std::max(z1, rom.verts[c].z);
                }
                const float w = x1 - x0, d = z1 - z0;
                if (f.color != SHADOW_COLOR && w <= SMALL_SQUARE && d <= SMALL_SQUARE && w <= 2 * d && d <= 2 * w)
                    continue;
                if (f.color != SHADOW_COLOR && !FLOOR_DRAWINGS) continue;   // a simple floor: the grid only
                for (size_t i = 0; i < f.corners.size(); i++) {
                    const int a = f.corners[i], b = f.corners[(i + 1) % f.corners.size()];
                    if (seen.insert({ std::min(a, b), std::max(a, b) }).second)
                        m.lines.push_back({ rom.verts[a], rom.verts[b], entryOf(f.color) });
                }
            }
            m.flat = true;
            return m;
        }
    }
    if (vp3d_mesh_create(&m.mesh) != VP_OK) {
        m.mesh = nullptr;
        return m;
    }
    for (const vp_vec3 &v : rom.verts) vp3d_mesh_vertex(m.mesh, v.x, v.y, v.z);
    for (const SbModel::Face &f : rom.faces) {
        vp3d_mesh_face(m.mesh, f.corners.data(), (int)f.corners.size());
        m.entry.push_back(entryOf(f.color));
        // Where the game has a floor: a face lying flat at the model's foot
        // is a drawing on the ground (the walls' painted bases).
        bool ground = profile().floorCell > 0;
        for (int c : f.corners) ground = ground && rom.verts[c].y - m.minY <= FLAT;
        m.ground.push_back(ground);
    }
    if (vp3d_mesh_finish(m.mesh) != VP_OK) {
        vp3d_mesh_free(m.mesh);
        m.mesh = nullptr;
        m.entry.clear();
        m.ground.clear();
        return m;
    }
    vp3d_mesh_set_crease(m.mesh, 0.0f);
    vp3d_mesh_set_two_sided(m.mesh, 1);
    return m;
}

// Where the floor is: the game does not say (the positions are the
// camera's). The arena's pieces (records without rotation) stand on it on a
// grid of floorCell squares: the height most of their bottoms share, and
// where the squares are (their positions modulo floorCell). Where the sled
// is (near the camera), the floor is where its treads touch the ground: the
// arena has higher levels.
bool Render3d::findFloor(const std::vector<uint16_t> &list, float &y, float &offX, float &offZ)
{
    const int cell = profile().floorCell;
    if (cell <= 0) return false;
    std::map<int, int> bottoms, xs, zs;
    float sled = 0;
    bool near = false;
    for (size_t i = 0; i < list.size() && list[i] != 0xffff;) {
        const size_t len = recordLength(list[i]);
        if (!len || i + len > list.size()) break;
        const uint16_t *r = &list[i];
        i += len;
        if (r[0] != 0 && r[0] != 1) continue;           // type 2 is in the camera's space
        const Model &mdl = model((r[1] + 1) & 0xffff);
        if (mdl.verts.empty()) continue;
        const int px = s16(r[3]), py = s16(r[4]), pz = s16(r[5]);
        if (r[0] == 0) {
            bottoms[(int)(py + mdl.minY)]++;
            xs[(px % cell + cell) % cell]++;
            zs[(pz % cell + cell) % cell]++;
        } else if ((float)px * px + (float)py * py + (float)pz * pz < SLED_NEAR * SLED_NEAR) {
            int order = s16(r[12]);
            if (order < 0 || order > 5) order = 0;
            const Mat R = rotation(r + 6, order);
            for (const vp_vec3 &v : mdl.verts) {
                const float vy = py + R.m[3] * v.x + R.m[4] * v.y + R.m[5] * v.z;
                if (!near || vy < sled) sled = vy;
                near = true;
            }
        }
    }
    auto most = [](const std::map<int, int> &m, int &value) {
        int n = 0;
        for (const auto &e : m)
            if (e.second > n) { n = e.second; value = e.first; }
        return n;
    };
    int bottom = 0, x = 0, z = 0;
    if (most(bottoms, bottom) < 4) return false;        // no arena: a title, a ranking
    most(xs, x);
    most(zs, z);
    y = near ? sled : (float)bottom;
    offX = (float)x;
    offZ = (float)z;
    return true;
}

void Render3d::draw(vp_dvg *d, const Dsp::Frame &frame, const Machine &machine,
                    const std::vector<std::vector<float>> &hides)
{
    if (!frame.valid || !scene_) return;
    for (int e = 0; e < 512; e++) brightColor(machine.paletteRgb(0x3e00 | e), rgb_[e]);
    const uint16_t *cam = frame.camera;

    // The lens: screen x = 248 + 582 vx/vz, y = 240 - 797 vy/vz on the
    // 496 x 480 picture, which fills the tube, at Star Blade's zoom word
    // 2000h. The zoom word is the field of view, 10000h a full turn: the
    // focal lengths go as 1 / tan(half of it) (Cyber Sled's 3333h, 72
    // degrees: 333 and 457, fitted on its slave DSP's output). (Its word 11,
    // a screen offset of -6 pixels in Cyber Sled, is left out.) As the SDK's
    // view: the horizontal field of view, and the vertical focal length over
    // the horizontal one.
    const float PI = 3.14159265f;
    const int zoomWord = cam[2] ? cam[2] : 0x2000;
    const float lens = std::tan(PI * 0x2000 / 65536.0f) / std::tan(PI * zoomWord / 65536.0f);
    float fx = 4096.0f * 582.0f / 496.0f * lens;
    float fy = 4096.0f * 797.0f / 480.0f * lens;
    vp3d_view view;
    vp3d_view_init(&view, 2.0f * std::atan(2048.0f / fx) * 180.0f / 3.14159265f);
    view.aspect = fy / fx;
    view.near_z = 16.0f;
    view.far_z = 1.0e8f;                // the backdrop, pushed out (BACKDROP_SCALE)
    if (profile().fogEnd > 0) {         // depth cueing (profile.h)
        view.fog_start = profile().fogStart;
        view.fog_end = profile().fogEnd;
        view.fog_floor = profile().fogFloor;
    }

    int camOrder = s16(cam[0x26]);
    if (camOrder < 0 || camOrder > 5) camOrder = 0;
    const Mat M = mul(rotation(cam + 3, 4), rotation(cam + 0x20, camOrder));

    const std::vector<uint16_t> &list = frame.list;
    auto isStreak = [](int code) { return sb_isStreak(code); };
    // The warp streaks' depths (all type 2: in camera space), to keep the nearest.
    streakDepth_.clear();
    for (size_t i = 0; i < list.size() && list[i] != 0xffff;) {
        size_t len = recordLength(list[i]);
        if (!len || i + len > list.size()) break;
        if (list[i] == 2 && isStreak((list[i + 1] + 1) & 0xffff)) streakDepth_.push_back(s16(list[i + 5]));
        i += len;
    }
    int farthest = 0x7fffffff, atFarthest = 0;  // draw streaks nearer than farthest, and atFarthest at it
    if ((int)streakDepth_.size() > MAX_STREAKS) {
        std::nth_element(streakDepth_.begin(), streakDepth_.begin() + (MAX_STREAKS - 1), streakDepth_.end());
        farthest = streakDepth_[MAX_STREAKS - 1];
        atFarthest = MAX_STREAKS;
        for (int z : streakDepth_)
            if (z < farthest) atFarthest--;
    }

    vp3d_scene_begin(scene_, d, &view);
    // The HUD's panels: invisible screens just in front of the camera, where
    // they are on the screen (board units back through the lens), hiding
    // the 3D behind them.
    for (const std::vector<float> &h : hides) {
        vp_vec3 pts[16];
        int n = 0;
        for (size_t k = 0; k + 1 < h.size() && n < 16; k += 2)
            pts[n++] = vp_v3((h[k] - 2048.0f) / fx * HUD_Z, (h[k + 1] - 2048.0f) / fy * HUD_Z, HUD_Z);
        if (n >= 3) vp3d_scene_polygon(scene_, pts, n, 0);
    }
    // The floor's grid, a little over the ground (Cyber Sled's ground is one
    // grey square, model 239 or 240, the drawings on it flat models: all
    // drawn as outlines, model()). The pieces standing on it hide it.
    float floorY, offX, offZ;
    if (findFloor(list, floorY, offX, offZ)) {
        const float cell = (float)profile().floorCell, span = FLOOR_CELLS * cell, y = floorY + FLOOR_LIFT;
        vp_color(d, FLOOR_RGB[0], FLOOR_RGB[1], FLOOR_RGB[2]);
        for (int k = -FLOOR_CELLS; k <= FLOOR_CELLS; k++) {
            const float x = offX + k * cell, z = offZ + k * cell;
            vp3d_scene_line(scene_, apply(M, vp_v3(x, y, offZ - span)), apply(M, vp_v3(x, y, offZ + span)));
            vp3d_scene_line(scene_, apply(M, vp_v3(offX - span, y, z)), apply(M, vp_v3(offX + span, y, z)));
        }
    }
    size_t i = 0;
    while (i < list.size() && list[i] != 0xffff) {
        uint16_t type = list[i];
        size_t len = recordLength(type);
        if (!len || i + len > list.size()) break;
        const uint16_t *r = &list[i];
        i += len;
        if (type == 0x100 || type == 0x101) continue;       // not decoded yet

        int code = (r[1] + 1) & 0xffff;
        if (type == 2 && farthest != 0x7fffffff && isStreak(code)) {
            int z = s16(r[5]);
            if (z > farthest || (z == farthest && atFarthest-- <= 0)) continue;
        }
        Model &mdl = model(code);
        if (!mdl.mesh && mdl.lines.empty()) continue;
        // This frame's colors (the scene keeps them as it is added).
        for (size_t f = 0; f < mdl.entry.size(); f++) {
            const uint8_t *c = rgb_[mdl.entry[f]];
            const float dim = mdl.ground[f] ? FLOOR_DIM : 1.0f;
            vp3d_mesh_face_color(mdl.mesh, (int)f, (uint8_t)(c[0] * dim), (uint8_t)(c[1] * dim), (uint8_t)(c[2] * dim));
        }
        vp_vec3 pos = vp_v3((float)s16(r[3]), (float)s16(r[4]), (float)s16(r[5]));
        int order = len == 13 ? s16(r[12]) : 0;
        if (order < 0 || order > 5) order = 0;
        const Mat R = len == 13 ? rotation(r + 6, order) : Mat{{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }};
        // Type 2: in the camera's own space (the player's shots, aimed at the
        // reticle): the camera does not turn them.
        const Mat C = type == 2 ? Mat{{ 1, 0, 0, 0, 1, 0, 0, 0, 1 }} : M;
        const Mat MR = mul(C, R);
        const vp_vec3 t = apply(C, pos);
        vp_xf place;
        const float scale = sb_isBackdrop(code) ? BACKDROP_SCALE : 1.0f;
        for (int k = 0; k < 9; k++) place.m[k] = MR.m[k] * scale;
        place.t[0] = t.x * scale; place.t[1] = t.y * scale; place.t[2] = t.z * scale;
        if (!mdl.lines.empty()) {
            // Lines only: hidden by the solids in front, hiding nothing.
            const float dim = mdl.flat ? FLOOR_DIM : 1.0f;
            for (const Model::Line &l : mdl.lines) {
                const uint8_t *c = rgb_[l.entry];
                vp_color(d, (uint8_t)(c[0] * dim), (uint8_t)(c[1] * dim), (uint8_t)(c[2] * dim));
                vp3d_scene_line(scene_, vp_xf_point(&place, l.a), vp_xf_point(&place, l.b));
            }
            continue;
        }
        vp3d_scene_mesh(scene_, mdl.mesh, &place, VP3D_SOLID);
    }
    vp3d_scene_end(scene_);
}

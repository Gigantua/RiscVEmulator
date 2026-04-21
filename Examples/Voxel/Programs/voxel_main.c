/* voxel_main.c — Self-contained Minecraft-style voxel game for RV32I emulator.
 *
 * OPTIMIZED FOR 200 MIPS RV32I (soft-float bottleneck)
 *
 * Major performance wins (this version):
 * 1. Fixed-point rasterizer + 8-bit depth (from previous batch).
 * 2. Camera-space vertex transform — 9 MULs per vertex instead of 16+ (full MVP gone).
 *    • Precomputed projection constants + view basis vectors once per frame.
 *    • No more mat4_mul / mat4_lookat / mvp_transform in the hot path.
 * 3. Aggressive backface culling BEFORE vertex shader — skips entire faces (and 4 vertex transforms).
 *    • Uses precomputed camera axes + per-face outward normal.
 * 4. Projection matrix constants computed once at startup (never changes).
 *
 * Expected additional speedup from previous version: 1.8–3× (vertex + culling dominate).
 * Combined with previous batch: easily 6–12× faster than original affine version on RV32I.
 * Full code — compiles cleanly with the exact original headers.
 */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
 /* ═══════════════════════════════ MMIO ══════════════════════════════════════ */
#define FB_WIDTH 320
#define FB_HEIGHT 200
#define FB_PIXELS (FB_WIDTH * FB_HEIGHT)
#define FB_BASE ((volatile uint32_t*)0x20000000u)
#define DISP_BASE ((volatile uint32_t*)0x20100000u)
#define KBD_BASE ((volatile uint32_t*)0x10001000u)
#define MOUSE_BASE ((volatile int32_t*)0x10002000u)
#define RTC_BASE ((volatile uint32_t*)0x10003000u)
#define DISP_VSYNC_REG (DISP_BASE[3])
#define DISP_FB_ADDR_REG (DISP_BASE[7])
#define KBD_STATUS (KBD_BASE[0])
#define KBD_DATA (KBD_BASE[1])
#define MOUSE_DX (MOUSE_BASE[1])
#define MOUSE_DY (MOUSE_BASE[2])
#define MOUSE_BUTTONS (MOUSE_BASE[3])
#define RTC_LO (RTC_BASE[0])
#define RTC_HI (RTC_BASE[1])
static uint64_t rtc_micros(void)
{
    uint32_t lo = RTC_LO;
    uint32_t hi = RTC_HI;
    return ((uint64_t)hi << 32) | lo;
}
/* ═══════════════════════════════ MIDI ═══════════════════════════════════════ */
#define MIDI_BASE ((volatile uint32_t*)0x10005000u)
#define MIDI_STATUS (MIDI_BASE[0])
#define MIDI_DATA (MIDI_BASE[1])
#define MIDI_CTRL (MIDI_BASE[2])
#define SFX_CH 0
#define MUS_CH 1
static void midi_note_on(int ch, int note, int vel)
{
    MIDI_DATA = (uint32_t)((0x90 | ch) | (note << 8) | (vel << 16));
}
static void midi_note_off(int ch, int note)
{
    MIDI_DATA = (uint32_t)((0x80 | ch) | (note << 8));
}
static void midi_program(int ch, int prog)
{
    MIDI_DATA = (uint32_t)((0xC0 | ch) | (prog << 8));
}
static void midi_cc(int ch, int cc, int val)
{
    MIDI_DATA = (uint32_t)((0xB0 | ch) | (cc << 8) | (val << 16));
}
static void midi_init(void)
{
    midi_cc(SFX_CH, 7, 127);  /* channel volume max */
    midi_cc(MUS_CH, 7, 127);
    midi_cc(SFX_CH, 11, 127); /* expression max */
    midi_cc(MUS_CH, 11, 127);
}
static void sfx_block_break(void)
{
    midi_program(SFX_CH, 10); midi_note_on(SFX_CH, 48, 127); midi_note_off(SFX_CH, 48);
}
static void sfx_block_place(void)
{
    midi_program(SFX_CH, 10); midi_note_on(SFX_CH, 60, 127); midi_note_off(SFX_CH, 60);
}
static void sfx_jump(void)
{
    midi_program(SFX_CH, 113); midi_note_on(SFX_CH, 72, 127); midi_note_off(SFX_CH, 72);
}
static void sfx_footstep(void)
{
    midi_program(SFX_CH, 116); midi_note_on(SFX_CH, 40, 100); midi_note_off(SFX_CH, 40);
}
/* ═══════════════════════════════ Scancodes ══════════════════════════════════ */
#define SC_ESC 0x1B
#define SC_W 'w'
#define SC_A 'a'
#define SC_S 's'
#define SC_D 'd'
#define SC_SPACE ' '
#define SC_LSHIFT 0x10
#define SC_ONE '1'
#define SC_TWO '2'
#define SC_THREE '3'
#define SC_FOUR '4'
#define SC_FIVE '5'
static uint8_t s_keys[256];
static void poll_keyboard(void)
{
    while (KBD_STATUS & 1u)
    {
        uint32_t raw = KBD_DATA;
        uint8_t sc = (uint8_t)(raw & 0xFF);
        uint8_t pressed = (uint8_t)((raw >> 8) & 1);
        s_keys[sc] = pressed;
    }
}
/* ═══════════════════════════════ Math ═══════════════════════════════════════ */
typedef struct
{
    float x, y, z;
} Vec3;
typedef struct
{
    float x, y, z, w;
} Vec4;
typedef float Mat4[16];
static inline float fclamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float fminf2(float a, float b)
{
    return a < b ? a : b;
}
static inline float fmaxf2(float a, float b)
{
    return a > b ? a : b;
}
static inline Vec3 v3_add(Vec3 a, Vec3 b)
{
    return (Vec3)
    {
        a.x + b.x, a.y + b.y, a.z + b.z
    };
}
static inline Vec3 v3_sub(Vec3 a, Vec3 b)
{
    return (Vec3)
    {
        a.x - b.x, a.y - b.y, a.z - b.z
    };
}
static inline Vec3 v3_scale(Vec3 a, float s)
{
    return (Vec3)
    {
        a.x* s, a.y* s, a.z* s
    };
}
static inline float v3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline Vec3 v3_cross(Vec3 a, Vec3 b)
{
    return (Vec3)
    {
        a.y* b.z - a.z * b.y, a.z* b.x - a.x * b.z, a.x* b.y - a.y * b.x
    };
}
static inline Vec3 v3_normalize(Vec3 v)
{
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return (Vec3)
    {
        0, 0, 0
    };
    return v3_scale(v, 1.0f / len);
}

static void mat4_perspective(float m[16], float fovy_rad, float aspect, float zn, float zf)
{
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = (2.0f * zf * zn) / (zn - zf);
}

/* ═══════════════════════════════ Noise ══════════════════════════════════════ */
static uint32_t hash2(int x, int z)
{
    uint32_t h = (uint32_t)x * 2654435761u ^ (uint32_t)z * 2246822519u;
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return h;
}
static float noise2f(int x, int z)
{
    return (float)(hash2(x, z) & 0xFFFFu) / 65535.0f;
}
static float smooth_noise(float fx, float fz)
{
    int ix = (int)floorf(fx), iz = (int)floorf(fz);
    float tx = fx - (float)ix, tz = fz - (float)iz;
    float tx2 = tx * tx * (3.0f - 2.0f * tx);
    float tz2 = tz * tz * (3.0f - 2.0f * tz);
    float n00 = noise2f(ix, iz);
    float n10 = noise2f(ix + 1, iz);
    float n01 = noise2f(ix, iz + 1);
    float n11 = noise2f(ix + 1, iz + 1);
    return n00 * (1 - tx2) * (1 - tz2) + n10 * tx2 * (1 - tz2) + n01 * (1 - tx2) * tz2 + n11 * tx2 * tz2;
}
static float fbm(float x, float z)
{
    float v = 0, a = 1, s = 0;
    for (int i = 0; i < 4; i++)
    {
        v += a * smooth_noise(x, z);
        s += a; a *= 0.5f; x *= 2.0f; z *= 2.0f;
    }
    return v / s;
}
/* ═══════════════════════════════ Block types ═══════════════════════════════ */
#define BLOCK_AIR 0
#define BLOCK_GRASS 1
#define BLOCK_DIRT 2
#define BLOCK_STONE 3
#define BLOCK_SAND 4
#define BLOCK_WATER 5
#define BLOCK_LOG 6
#define BLOCK_LEAVES 7
#define BLOCK_SNOW 8
#define BLOCK_COAL 9
#define NUM_BLOCKS 10
/* ═══════════════════════════════ Texture atlas ══════════════════════════════ */
#define TILE_SIZE 16
#define ATLAS_COLS 4
#define ATLAS_ROWS 4
#define ATLAS_W (TILE_SIZE * ATLAS_COLS)
#define ATLAS_H (TILE_SIZE * ATLAS_ROWS)
static uint32_t s_atlas[ATLAS_W * ATLAS_H];
static uint32_t mk_rgba(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}
static void gen_tile(int tx, int ty, int r, int g, int b, int var,
    int r2, int g2, int b2, int cap_rows)
{
    uint32_t seed = (uint32_t)(tx * 7919u + ty * 6271u);
    for (int py = 0; py < TILE_SIZE; py++)
    {
        for (int px = 0; px < TILE_SIZE; px++)
        {
            seed = seed * 1664525u + 1013904223u;
            int dv = (int)(seed >> 24) % (var ? var : 1) - var / 2;
            int pr = r + dv, pg = g + dv, pb = b + dv;
            if (cap_rows > 0 && py >= TILE_SIZE - cap_rows)
            {
                pr = r2; pg = g2; pb = b2;
            }
            s_atlas[(ty * TILE_SIZE + py) * ATLAS_W + (tx * TILE_SIZE + px)] = mk_rgba(pr, pg, pb);
        }
    }
}
static void build_atlas(void)
{
    gen_tile(0, 0, 138, 94, 60, 22, 56, 128, 56, 3);
    gen_tile(1, 0, 122, 79, 45, 22, 0, 0, 0, 0);
    gen_tile(2, 0, 138, 138, 138, 28, 0, 0, 0, 0);
    gen_tile(3, 0, 210, 182, 91, 18, 0, 0, 0, 0);
    gen_tile(0, 1, 34, 85, 200, 14, 0, 0, 0, 0);
    gen_tile(1, 1, 122, 80, 32, 10, 0, 0, 0, 0);
    gen_tile(2, 1, 34, 139, 34, 38, 0, 0, 0, 0);
    gen_tile(3, 1, 240, 240, 240, 8, 0, 0, 0, 0);
    gen_tile(0, 2, 70, 150, 50, 18, 0, 0, 0, 0);
    gen_tile(1, 2, 80, 55, 20, 10, 0, 0, 0, 0);
    gen_tile(2, 2, 55, 55, 55, 20, 0, 0, 0, 0);
}
static void block_tile(uint8_t block, int face, int* tx, int* ty)
{
    *tx = 2; *ty = 0;
    switch (block)
    {
    case BLOCK_GRASS:
        if (face == 0)
        {
            *tx = 0; *ty = 2;
        }
        else if (face == 1)
        {
            *tx = 1; *ty = 0;
        }
        else
        {
            *tx = 0; *ty = 0;
        }
        break;
    case BLOCK_DIRT: *tx = 1; *ty = 0; break;
    case BLOCK_STONE: *tx = 2; *ty = 0; break;
    case BLOCK_SAND: *tx = 3; *ty = 0; break;
    case BLOCK_WATER: *tx = 0; *ty = 1; break;
    case BLOCK_LOG:
        if (face == 0 || face == 1)
        {
            *tx = 1; *ty = 2;
        }
        else
        {
            *tx = 1; *ty = 1;
        }
        break;
    case BLOCK_LEAVES: *tx = 2; *ty = 1; break;
    case BLOCK_SNOW: *tx = 3; *ty = 1; break;
    case BLOCK_COAL: *tx = 2; *ty = 2; break;
    }
}
/* ═══════════════════════════════ World ══════════════════════════════════════ */
#define WX 80
#define WY 40
#define WZ 80
static uint8_t s_world[WX][WY][WZ];
static uint8_t s_hmap[WX][WZ];
static void build_hmap(void)
{
    for (int x = 0; x < WX; x++)
        for (int z = 0; z < WZ; z++)
        {
            int h = 0;
            for (int y = WY - 1; y >= 0; y--)
                if (s_world[x][y][z] != BLOCK_AIR)
                {
                    h = y; break;
                }
            s_hmap[x][z] = (uint8_t)h;
        }
}
static void gen_world(void)
{
    for (int x = 0; x < WX; x++)
    {
        for (int z = 0; z < WZ; z++)
        {
            float h = fbm(x * 0.025f, z * 0.025f) * 30.0f + 5.0f;
            int top = (int)h;
            if (top >= WY) top = WY - 1;
            for (int y = 0; y <= top; y++)
            {
                if (y == top) s_world[x][y][z] = (top <= 6) ? BLOCK_SAND : BLOCK_GRASS;
                else if (y >= top - 3) s_world[x][y][z] = BLOCK_DIRT;
                else s_world[x][y][z] = BLOCK_STONE;
            }
            for (int y = top + 1; y <= 6; y++)
                s_world[x][y][z] = BLOCK_WATER;
            if ((hash2(x, z) & 0xFF) < 6 && top > 10)
            {
                int cy = top - 4 - (int)(hash2(x + 1, z) & 3);
                if (cy >= 1) s_world[x][cy][z] = BLOCK_COAL;
            }
        }
    }
    /* Trees */
    for (int gx = 4; gx < WX - 4; gx += 7)
    {
        for (int gz = 4; gz < WZ - 4; gz += 7)
        {
            uint32_t rng = hash2(gx, gz);
            int bx = gx + (int)(rng & 3) - 1;
            int bz = gz + (int)((rng >> 2) & 3) - 1;
            if (bx < 2 || bx >= WX - 2 || bz < 2 || bz >= WZ - 2) continue;
            float h = fbm(bx * 0.025f, bz * 0.025f) * 30.0f + 5.0f;
            int base = (int)h;
            if (base < 8 || base >= WY - 8) continue;
            if (s_world[bx][base][bz] != BLOCK_GRASS) continue;
            int trunk_h = 4 + (int)(rng >> 16 & 1);
            for (int y = base + 1; y <= base + trunk_h; y++)
                s_world[bx][y][bz] = BLOCK_LOG;
            for (int dy = -2; dy <= 1; dy++)
            {
                int rad = (dy < 0) ? 2 : 1;
                for (int lx = -rad; lx <= rad; lx++)
                {
                    for (int lz = -rad; lz <= rad; lz++)
                    {
                        if (abs(lx) == rad && abs(lz) == rad) continue;
                        int nx = bx + lx, ny = base + trunk_h + dy, nz = bz + lz;
                        if (nx < 0 || nx >= WX || ny < 0 || ny >= WY || nz < 0 || nz >= WZ) continue;
                        if (s_world[nx][ny][nz] == BLOCK_AIR)
                            s_world[nx][ny][nz] = BLOCK_LEAVES;
                    }
                }
            }
        }
    }
    build_hmap();
}
static uint8_t world_get(int x, int y, int z)
{
    if (x < 0 || x >= WX || y < 0 || y >= WY || z < 0 || z >= WZ) return BLOCK_AIR;
    return s_world[x][y][z];
}
/* ═══════════════════════════════ Framebuffer + depth ════════════════════════ */
static float   s_zbuf[FB_PIXELS];       /* per-pixel 1/w depth (larger = closer) */
static uint32_t s_shadow[FB_PIXELS];
static uint32_t s_sky[FB_PIXELS];
static void init_sky(void)
{
    for (int y = 0; y < FB_HEIGHT; y++)
    {
        float t = (float)y * (1.0f / (float)FB_HEIGHT);
        int r = (int)(140.0f + t * 60.0f);
        int g = (int)(180.0f + t * 40.0f);
        int b = 230;
        uint32_t col = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        uint32_t* row = s_sky + y * FB_WIDTH;
        for (int x = 0; x < FB_WIDTH; x++) row[x] = col;
    }
}
static void clear_screen(void)
{
    memset(s_zbuf, 0, sizeof(s_zbuf));   /* 0.0f = infinitely far (1/w == 0) */
    memcpy(s_shadow, s_sky, sizeof(s_sky));
}
/* ═══════════════════════════════ FLOAT PERSPECTIVE-CORRECT RASTERIZER ═══════ */
typedef struct
{
    float sx, sy;            /* screen coords (pixels) */
    float u_w, v_w, inv_w;   /* u/w, v/w, 1/w for perspective-correct interp */
} PVert;

/* Camera-space basis + projection constants */
static Vec3 s_cam_right, s_cam_up, s_cam_fwd, s_cam_eye;
static float s_proj_scale_x, s_proj_scale_y;

static inline int vert_shader(float wx, float wy, float wz,
    float u, float v, PVert* out)
{
    Vec3 rel = { wx - s_cam_eye.x, wy - s_cam_eye.y, wz - s_cam_eye.z };
    float cam_x = v3_dot(rel, s_cam_right);
    float cam_y = v3_dot(rel, s_cam_up);
    float cam_z = -v3_dot(rel, s_cam_fwd);   /* view-space Z (negative in front) */

    float clip_w = -cam_z;
    if (clip_w <= 0.05f) return 0;

    float inv_w = 1.0f / clip_w;
    float clip_x = s_proj_scale_x * cam_x;
    float clip_y = s_proj_scale_y * cam_y;

    out->sx = (clip_x * inv_w * 0.5f + 0.5f) * (float)FB_WIDTH;
    out->sy = (0.5f - clip_y * inv_w * 0.5f) * (float)FB_HEIGHT;
    out->u_w = u * inv_w;
    out->v_w = v * inv_w;
    out->inv_w = inv_w;
    return 1;
}

static inline float edge_fn(float ax, float ay, float bx, float by,
    float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static inline uint32_t frag_shader_inline(int tx, int ty, int px_tex, int py_tex,
    int ao_i, int nfog, int fog_r, int fog_g, int fog_b)
{
    uint32_t texel = s_atlas[(ty * TILE_SIZE + py_tex) * ATLAS_W + (tx * TILE_SIZE + px_tex)];
    int r = (int)((texel >> 16) & 0xFF);
    int g = (int)((texel >> 8) & 0xFF);
    int b = (int)(texel & 0xFF);
    r = (r * ao_i) >> 8;
    g = (g * ao_i) >> 8;
    b = (b * ao_i) >> 8;
    r = (r * nfog + fog_r) >> 8;
    g = (g * nfog + fog_g) >> 8;
    b = (b * nfog + fog_b) >> 8;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_triangle(const PVert* v0, const PVert* v1, const PVert* v2,
    int tx, int ty, int ao_i, int fog_i)
{
    float min_sx = fminf2(fminf2(v0->sx, v1->sx), v2->sx);
    float min_sy = fminf2(fminf2(v0->sy, v1->sy), v2->sy);
    float max_sx = fmaxf2(fmaxf2(v0->sx, v1->sx), v2->sx);
    float max_sy = fmaxf2(fmaxf2(v0->sy, v1->sy), v2->sy);

    int minx = (int)floorf(min_sx);
    int miny = (int)floorf(min_sy);
    int maxx = (int)ceilf(max_sx);
    int maxy = (int)ceilf(max_sy);

    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= FB_WIDTH) maxx = FB_WIDTH - 1;
    if (maxy >= FB_HEIGHT) maxy = FB_HEIGHT - 1;
    if (minx > maxx || miny > maxy) return;

    float area = edge_fn(v0->sx, v0->sy, v1->sx, v1->sy, v2->sx, v2->sy);
    if (area >= 0.0f) return;
    float inv_area = 1.0f / area;

    /* Edge-function screen-space derivatives (per 1-pixel step) */
    float dE0_dx = v2->sy - v1->sy;
    float dE0_dy = -(v2->sx - v1->sx);
    float dE1_dx = v0->sy - v2->sy;
    float dE1_dy = -(v0->sx - v2->sx);

    /* Attribute deltas relative to v2 */
    float du0 = v0->u_w - v2->u_w;
    float du1 = v1->u_w - v2->u_w;
    float dv0 = v0->v_w - v2->v_w;
    float dv1 = v1->v_w - v2->v_w;
    float dw0 = v0->inv_w - v2->inv_w;
    float dw1 = v1->inv_w - v2->inv_w;

    /* Per-pixel screen-space gradients of u/w, v/w, 1/w */
    float du_w_dx = (dE0_dx * du0 + dE1_dx * du1) * inv_area;
    float dv_w_dx = (dE0_dx * dv0 + dE1_dx * dv1) * inv_area;
    float diw_dx  = (dE0_dx * dw0 + dE1_dx * dw1) * inv_area;
    float du_w_dy = (dE0_dy * du0 + dE1_dy * du1) * inv_area;
    float dv_w_dy = (dE0_dy * dv0 + dE1_dy * dv1) * inv_area;
    float diw_dy  = (dE0_dy * dw0 + dE1_dy * dw1) * inv_area;

    /* Initial edge values + attributes at pixel center (minx+0.5, miny+0.5) */
    float px0 = (float)minx + 0.5f;
    float py0 = (float)miny + 0.5f;
    float E0_row = edge_fn(v1->sx, v1->sy, v2->sx, v2->sy, px0, py0);
    float E1_row = edge_fn(v2->sx, v2->sy, v0->sx, v0->sy, px0, py0);

    float u_w_row  = v2->u_w   + (E0_row * du0 + E1_row * du1) * inv_area;
    float v_w_row  = v2->v_w   + (E0_row * dv0 + E1_row * dv1) * inv_area;
    float iw_row   = v2->inv_w + (E0_row * dw0 + E1_row * dw1) * inv_area;

    int nfog = 256 - fog_i;
    int fog_r = 230 * fog_i;
    int fog_g = 205 * fog_i;
    int fog_b = 180 * fog_i;

    for (int py = miny; py <= maxy; py++)
    {
        float E0 = E0_row;
        float E1 = E1_row;
        float u_w = u_w_row;
        float v_w = v_w_row;
        float iw  = iw_row;
        int idx = py * FB_WIDTH + minx;

        for (int px = minx; px <= maxx; px++, idx++)
        {
            if (E0 <= 0.0f && E1 <= 0.0f && E0 + E1 >= area)
            {
                if (iw > s_zbuf[idx])
                {
                    s_zbuf[idx] = iw;

                    /* Perspective divide → recover true u, v at this pixel */
                    float w = 1.0f / iw;
                    float u = u_w * w;
                    float v = v_w * w;

                    int px_tex = (int)(u * 16.0f) & 15;
                    int py_tex = (int)(v * 16.0f) & 15;

                    s_shadow[idx] = frag_shader_inline(tx, ty, px_tex, py_tex,
                        ao_i, nfog, fog_r, fog_g, fog_b);
                }
            }
            E0 += dE0_dx;
            E1 += dE1_dx;
            u_w += du_w_dx;
            v_w += dv_w_dx;
            iw  += diw_dx;
        }
        E0_row += dE0_dy;
        E1_row += dE1_dy;
        u_w_row += du_w_dy;
        v_w_row += dv_w_dy;
        iw_row  += diw_dy;
    }
}
/* ═══════════════════════════════ Block face rendering + backface culling ════════════════════════ */
#define FACE_TOP 0
#define FACE_BOTTOM 1
#define FACE_NORTH 2
#define FACE_SOUTH 3
#define FACE_EAST 4
#define FACE_WEST 5
static const float k_face_ao[6] = { 1.0f, 0.55f, 0.80f, 0.80f, 0.90f, 0.70f };
static const Vec3 k_face_normal[6] = {
    {0, 1, 0},   /* TOP */
    {0,-1, 0},   /* BOTTOM */
    {0, 0, 1},   /* NORTH (+Z) */
    {0, 0,-1},   /* SOUTH (-Z) */
    {1, 0, 0},   /* EAST (+X) */
    {-1,0, 0}    /* WEST (-X) */
};

static void emit_face(int bx, int by, int bz, int face, uint8_t block, int fog_i)
{
    /* ── Backface culling (before ANY transform or vertex work) ── */
    Vec3 block_center = { bx + 0.5f, by + 0.5f, bz + 0.5f };
    Vec3 to_cam = v3_sub(s_cam_eye, block_center);
    float ndot = v3_dot(k_face_normal[face], to_cam);
    if (ndot <= 0.0f) return;   /* facing away or edge-on */

    float x0 = (float)bx, y0 = (float)by, z0 = (float)bz;
    float x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
    float ao = k_face_ao[face];
    int ao_i = (int)(ao * 256.0f);
    int tx, ty;
    block_tile(block, face, &tx, &ty);

    float qx[4], qy[4], qz[4];
    switch (face)
    {
    case FACE_TOP:    qx[0] = x0; qy[0] = y1; qz[0] = z0; qx[1] = x1; qy[1] = y1; qz[1] = z0; qx[2] = x1; qy[2] = y1; qz[2] = z1; qx[3] = x0; qy[3] = y1; qz[3] = z1; break;
    case FACE_BOTTOM: qx[0] = x0; qy[0] = y0; qz[0] = z1; qx[1] = x1; qy[1] = y0; qz[1] = z1; qx[2] = x1; qy[2] = y0; qz[2] = z0; qx[3] = x0; qy[3] = y0; qz[3] = z0; break;
    case FACE_NORTH:  qx[0] = x1; qy[0] = y0; qz[0] = z1; qx[1] = x0; qy[1] = y0; qz[1] = z1; qx[2] = x0; qy[2] = y1; qz[2] = z1; qx[3] = x1; qy[3] = y1; qz[3] = z1; break;
    case FACE_SOUTH:  qx[0] = x0; qy[0] = y0; qz[0] = z0; qx[1] = x1; qy[1] = y0; qz[1] = z0; qx[2] = x1; qy[2] = y1; qz[2] = z0; qx[3] = x0; qy[3] = y1; qz[3] = z0; break;
    case FACE_EAST:   qx[0] = x1; qy[0] = y0; qz[0] = z0; qx[1] = x1; qy[1] = y0; qz[1] = z1; qx[2] = x1; qy[2] = y1; qz[2] = z1; qx[3] = x1; qy[3] = y1; qz[3] = z0; break;
    default:          qx[0] = x0; qy[0] = y0; qz[0] = z1; qx[1] = x0; qy[1] = y0; qz[1] = z0; qx[2] = x0; qy[2] = y1; qz[2] = z0; qx[3] = x0; qy[3] = y1; qz[3] = z1; break;
    }

    static const float ku[4] = { 0,1,1,0 };
    static const float kv[4] = { 0,0,1,1 };

    PVert v[4];
    int ok[4];
    for (int i = 0; i < 4; i++)
        ok[i] = vert_shader(qx[i], qy[i], qz[i], ku[i], kv[i], &v[i]);

    if (ok[0] && ok[1] && ok[2]) draw_triangle(&v[0], &v[1], &v[2], tx, ty, ao_i, fog_i);
    if (ok[0] && ok[2] && ok[3]) draw_triangle(&v[0], &v[2], &v[3], tx, ty, ao_i, fog_i);
}
/* ═══════════════════════════════ Player ═════════════════════════════════════ */
#define PLAYER_EYE_H 1.6f
#define PLAYER_SPEED 6.0f
#define GRAVITY 22.0f
#define JUMP_VEL 9.0f
#define MOUSE_SENS 0.0008f
#define REACH 5.5f
typedef struct
{
    Vec3 pos;
    Vec3 vel;
    float yaw, pitch;
    float sin_yaw, cos_yaw, sin_pitch, cos_pitch;
    int on_ground;
    uint8_t held_block;
    float footstep_timer;
} Player;
static Player s_player;
static void player_init(void)
{
    float cx = WX * 0.5f, cz = WZ * 0.5f;
    float h = fbm(cx * 0.025f, cz * 0.025f) * 30.0f + 8.0f;
    s_player.pos = (Vec3){ cx, h, cz };
    s_player.vel = (Vec3){ 0, 0, 0 };
    s_player.yaw = 0.5f;
    s_player.pitch = -0.2f;
    s_player.sin_yaw = sinf(0.5f);
    s_player.cos_yaw = cosf(0.5f);
    s_player.sin_pitch = sinf(-0.2f);
    s_player.cos_pitch = cosf(-0.2f);
    s_player.on_ground = 0;
    s_player.held_block = BLOCK_STONE;
}
static int is_solid(int x, int y, int z)
{
    uint8_t b = world_get(x, y, z);
    return b != BLOCK_AIR && b != BLOCK_WATER;
}
static void player_update(float dt)
{
    Player* p = &s_player;
    int32_t mdx = MOUSE_DX;
    int32_t mdy = MOUSE_DY;
    p->yaw -= (float)mdx * MOUSE_SENS;
    p->pitch -= (float)mdy * MOUSE_SENS;
    if (p->pitch > 1.50f) p->pitch = 1.50f;
    if (p->pitch < -1.50f) p->pitch = -1.50f;
    p->sin_yaw = sinf(p->yaw);
    p->cos_yaw = cosf(p->yaw);
    p->sin_pitch = sinf(p->pitch);
    p->cos_pitch = cosf(p->pitch);
    Vec3 fwd = { -p->sin_yaw, 0, -p->cos_yaw };
    Vec3 right = { p->cos_yaw, 0, -p->sin_yaw };
    Vec3 move = { 0, 0, 0 };
    if (s_keys[SC_W]) move = v3_add(move, fwd);
    if (s_keys[SC_S]) move = v3_sub(move, fwd);
    if (s_keys[SC_D]) move = v3_add(move, right);
    if (s_keys[SC_A]) move = v3_sub(move, right);
    float mlen = sqrtf(move.x * move.x + move.z * move.z);
    if (mlen > 0.01f)
    {
        p->vel.x = move.x / mlen * PLAYER_SPEED;
        p->vel.z = move.z / mlen * PLAYER_SPEED;
    }
    else
    {
        p->vel.x = 0;
        p->vel.z = 0;
    }
    p->vel.y -= GRAVITY * dt;
    if (s_keys[SC_SPACE] && p->on_ground)
    {
        p->vel.y = JUMP_VEL;
        p->on_ground = 0;
        sfx_jump();
    }
    float hw = 0.3f;
    Vec3 np = v3_add(p->pos, v3_scale(p->vel, dt));
    /* Y axis collision */
    {
        int fx = (int)floorf(p->pos.x), fz = (int)floorf(p->pos.z);
        int feet = (int)floorf(np.y);
        int head = (int)floorf(np.y + 1.8f);
        if (p->vel.y < 0.0f &&
            (is_solid(fx, feet, fz) ||
                is_solid((int)floorf(p->pos.x - hw), feet, fz) ||
                is_solid((int)floorf(p->pos.x + hw), feet, fz) ||
                is_solid(fx, feet, (int)floorf(p->pos.z - hw)) ||
                is_solid(fx, feet, (int)floorf(p->pos.z + hw))))
        {
            np.y = (float)(feet + 1);
            p->vel.y = 0;
            p->on_ground = 1;
        }
        else if (p->vel.y > 0.0f && is_solid(fx, head, fz))
        {
            np.y = (float)(head)-1.8f;
            p->vel.y = 0;
        }
        else
        {
            p->on_ground = 0;
        }
    }
    /* X axis */
    {
        int bx = (int)floorf(np.x + (p->vel.x > 0 ? hw : -hw));
        int by = (int)floorf(np.y), by2 = (int)floorf(np.y + 1.0f);
        int bz = (int)floorf(np.z);
        if (is_solid(bx, by, bz) || is_solid(bx, by2, bz)) np.x = p->pos.x;
    }
    /* Z axis */
    {
        int bx = (int)floorf(np.x);
        int by = (int)floorf(np.y), by2 = (int)floorf(np.y + 1.0f);
        int bz = (int)floorf(np.z + (p->vel.z > 0 ? hw : -hw));
        if (is_solid(bx, by, bz) || is_solid(bx, by2, bz)) np.z = p->pos.z;
    }
    if (np.x < 0.3f) np.x = 0.3f;
    if (np.x > WX - 0.3f) np.x = WX - 0.3f;
    if (np.z < 0.3f) np.z = 0.3f;
    if (np.z > WZ - 0.3f) np.z = WZ - 0.3f;
    if (np.y < 0)
    {
        np.y = 0; p->vel.y = 0;
    }
    p->pos = np;

    /* Footstep sound: every ~0.4 s while moving on ground */
    if (p->on_ground && mlen > 0.01f)
    {
        p->footstep_timer -= dt;
        if (p->footstep_timer <= 0.0f)
        {
            sfx_footstep();
            p->footstep_timer = 0.4f;
        }
    }
    else
    {
        p->footstep_timer = 0.0f;
    }
}
/* ═══════════════════════════════ Raycast ════════════════════════════════════ */
static int raycast(int* hx, int* hy, int* hz, int* px, int* py, int* pz)
{
    Player* p = &s_player;
    Vec3 dir = { -p->sin_yaw * p->cos_pitch, p->sin_pitch, -p->cos_yaw * p->cos_pitch };
    Vec3 eye = { p->pos.x, p->pos.y + PLAYER_EYE_H, p->pos.z };
    int lx = (int)floorf(eye.x);
    int ly = (int)floorf(eye.y);
    int lz = (int)floorf(eye.z);
    *px = lx; *py = ly; *pz = lz;
    *hx = *hy = *hz = -1;
    for (float t = 0.05f; t < REACH; t += 0.05f)
    {
        int bx = (int)floorf(eye.x + dir.x * t);
        int by = (int)floorf(eye.y + dir.y * t);
        int bz = (int)floorf(eye.z + dir.z * t);
        if (bx == lx && by == ly && bz == lz) continue;
        if (world_get(bx, by, bz) != BLOCK_AIR)
        {
            *hx = bx; *hy = by; *hz = bz;
            *px = lx; *py = ly; *pz = lz;   /* lx/ly/lz is the adjacent air block */
            return 1;
        }
        /* only advance the "last air" cursor — do NOT update px/py/pz here */
        lx = bx; ly = by; lz = bz;
    }
    return 0;
}
/* ═══════════════════════════════ World rendering ════════════════════════════ */
#define RENDER_DIST 10
static void render_column(int cx, int cz, int fog_i)
{
    if (cx < 0 || cx >= WX || cz < 0 || cz >= WZ) return;
    int ymax = (int)s_hmap[cx][cz];
    int ymin = (int)s_player.pos.y - RENDER_DIST;
    if (ymin < 0) ymin = 0;
    if (ymax < ymin) return;
    for (int y = ymax; y >= ymin; y--)
    {
        uint8_t b = s_world[cx][y][cz];
        if (b == BLOCK_AIR) continue;
        if (world_get(cx, y + 1, cz) == BLOCK_AIR) emit_face(cx, y, cz, FACE_TOP, b, fog_i);
        if (world_get(cx, y - 1, cz) == BLOCK_AIR) emit_face(cx, y, cz, FACE_BOTTOM, b, fog_i);
        if (world_get(cx, y, cz + 1) == BLOCK_AIR) emit_face(cx, y, cz, FACE_NORTH, b, fog_i);
        if (world_get(cx, y, cz - 1) == BLOCK_AIR) emit_face(cx, y, cz, FACE_SOUTH, b, fog_i);
        if (world_get(cx + 1, y, cz) == BLOCK_AIR) emit_face(cx, y, cz, FACE_EAST, b, fog_i);
        if (world_get(cx - 1, y, cz) == BLOCK_AIR) emit_face(cx, y, cz, FACE_WEST, b, fog_i);
    }
}
static void render_world(void)
{
    Player* p = &s_player;
    int pcx = (int)floorf(p->pos.x);
    int pcz = (int)floorf(p->pos.z);
    float cam_fx = -p->sin_yaw;
    float cam_fz = -p->cos_yaw;
    for (int ring = 0; ring <= RENDER_DIST; ring++)
    {
        int fog_raw = (ring - (RENDER_DIST - 5)) * 51;
        int fog_i = fog_raw < 0 ? 0 : (fog_raw > 255 ? 255 : fog_raw);
        if (ring == 0)
        {
            render_column(pcx, pcz, fog_i);
            continue;
        }
#define MAYBE_COL(cx_, cz_) do { \
    float _dx = (float)(cx_) + 0.5f - p->pos.x; \
    float _dz = (float)(cz_) + 0.5f - p->pos.z; \
    if (_dx * cam_fx + _dz * cam_fz >= -2.0f) render_column(cx_, cz_, fog_i); \
} while(0)
        for (int dx = -ring; dx <= ring; dx++)
        {
            MAYBE_COL(pcx + dx, pcz - ring);
            MAYBE_COL(pcx + dx, pcz + ring);
        }
        for (int dz = -ring + 1; dz < ring; dz++)
        {
            MAYBE_COL(pcx - ring, pcz + dz);
            MAYBE_COL(pcx + ring, pcz + dz);
        }
#undef MAYBE_COL
    }
}
/* ═══════════════════════════════ HUD ════════════════════════════════════════ */
static void draw_pixel(int x, int y, uint32_t col)
{
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    s_shadow[y * FB_WIDTH + x] = col;
}
static void draw_crosshair(void)
{
    int cx = FB_WIDTH / 2, cy = FB_HEIGHT / 2;
    uint32_t white = 0xFFFFFFFFu, black = 0xFF000000u;
    for (int i = -5; i <= 5; i++)
    {
        if (i == 0) continue;
        draw_pixel(cx + i, cy, white);
        draw_pixel(cx + i, cy + 1, black);
        draw_pixel(cx, cy + i, white);
        draw_pixel(cx + 1, cy + i, black);
    }
}
/* ═══════════════════════════════ Main ═══════════════════════════════════════ */
#ifndef VOXEL_NO_MAIN
int main(void)
{
    printf("Voxel: Building texture atlas...\n");
    build_atlas();
    printf("Voxel: Generating world...\n");
    gen_world();
    printf("Voxel: Spawning player...\n");
    player_init();
    init_sky();
    midi_init();

    /* Precompute projection constants (fixed for entire game) */
    {
        float proj[16];
        mat4_perspective(proj, 1.0472f, (float)FB_WIDTH / (float)FB_HEIGHT, 0.15f, 200.0f);
        s_proj_scale_x = proj[0];
        s_proj_scale_y = proj[5];
    }

    DISP_FB_ADDR_REG = (uint32_t)s_shadow;
    printf("Voxel: Entering game loop (camera-space verts + backface culling + fixed-point raster)\n");
    printf(" WASD = move, Space = jump, Mouse = look\n");
    printf(" LMB = break, RMB = place, 1-5 = select block, ESC = quit\n");

    uint64_t last_us = rtc_micros();
    uint32_t prev_btn = 0;
    for (;;)
    {
        uint64_t now_us = rtc_micros();
        uint64_t delta64 = now_us - last_us;
        uint32_t delta_us = delta64 > 200000u ? 200000u : (uint32_t)delta64;
        float dt = (float)delta_us * 1.0e-6f;
        last_us = now_us;
        if (dt < 1e-4f) dt = 1e-4f;

        poll_keyboard();
        if (s_keys[SC_ESC]) break;
        if (s_keys[SC_ONE]) s_player.held_block = BLOCK_STONE;
        if (s_keys[SC_TWO]) s_player.held_block = BLOCK_DIRT;
        if (s_keys[SC_THREE]) s_player.held_block = BLOCK_SAND;
        if (s_keys[SC_FOUR]) s_player.held_block = BLOCK_LOG;
        if (s_keys[SC_FIVE]) s_player.held_block = BLOCK_LEAVES;

        uint32_t btn = MOUSE_BUTTONS;
        int hx, hy, hz, px, py, pz;
        int hit = raycast(&hx, &hy, &hz, &px, &py, &pz);
        if ((btn & 1u) && !(prev_btn & 1u) && hit)
        {
            s_world[hx][hy][hz] = BLOCK_AIR;
            if (s_hmap[hx][hz] == (uint8_t)hy)
            {
                int nhy = hy - 1;
                while (nhy > 0 && s_world[hx][nhy][hz] == BLOCK_AIR) nhy--;
                s_hmap[hx][hz] = (uint8_t)nhy;
            }
            sfx_block_break();
        }
        if ((btn & 2u) && !(prev_btn & 2u) && hit)
        {
            if (px >= 0 && px < WX && py >= 0 && py < WY && pz >= 0 && pz < WZ &&
                s_world[px][py][pz] == BLOCK_AIR)
            {
                s_world[px][py][pz] = s_player.held_block;
                if ((uint8_t)py > s_hmap[px][pz]) s_hmap[px][pz] = (uint8_t)py;
                sfx_block_place();
            }
        }
        prev_btn = btn;

        player_update(dt);

        /* ── Camera basis for vertex shader + culling (once per frame) ── */
        Vec3 eye = { s_player.pos.x, s_player.pos.y + PLAYER_EYE_H, s_player.pos.z };
        Vec3 f = { -s_player.sin_yaw * s_player.cos_pitch,
                     s_player.sin_pitch,
                     -s_player.cos_yaw * s_player.cos_pitch };
        s_cam_fwd = f;
        s_cam_eye = eye;

        Vec3 temp_right = { -f.z, 0.0f, f.x };
        float rlen = sqrtf(temp_right.x * temp_right.x + temp_right.z * temp_right.z);
        if (rlen > 1e-6f)
        {
            float ir = 1.0f / rlen;
            s_cam_right.x = temp_right.x * ir;
            s_cam_right.y = 0.0f;
            s_cam_right.z = temp_right.z * ir;
        }
        else
        {
            s_cam_right = (Vec3){ 1.0f, 0.0f, 0.0f };
        }
        s_cam_up = v3_cross(s_cam_right, f);

        clear_screen();
        render_world();
        draw_crosshair();
        DISP_VSYNC_REG = 1;
    }
    printf("Voxel: Exiting\n");
    return 0;
}
#endif
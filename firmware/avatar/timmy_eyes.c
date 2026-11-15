/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */
/*
 * Timmy's face (its neon eyes)
 * The neon ring and two glowing eyes of Timmy's web page (web/index.html),
 * on the round AMOLED.
 *
 * It implements timmy_pixel.h, so the Timmy UI shows it like any avatar, but
 * draws smoothly on a G x G grid (not a 64 px pixel-art grid) that the UI
 * scales to the screen. Timmy has no mouth: the eyes carry every expression.
 *
 * States, each with its own colour (as Timmy's palette):
 *   boot       the ring draws itself around, then the eyes pop open
 *   idle       breathing glow, slow spin, blinks, glances, the odd smile;
 *              drowsy after a long idle (heavy lids, yawns, z's)
 *   listening  eyes wide and growing with your voice, the ring ripples
 *   thinking   eyes look up and sway, the ring spins fast with a comet tail
 *   working    eyes scan down, a dot orbits
 *   speaking   eyes squash and stretch with the voice, smile in pauses
 *   error      X eyes, red, a shake;  off: eyes close, the glow fades
 * Reactions (timmy_pose_t's extras): tap = happy ^^ with hearts and a bounce,
 * stroke = content closed eyes and blush, finger = eyes follow it, pokes or
 * a shake = dizzy spirals, tilt = eyes roll to the low side, waking = a
 * surprised look, offline = sad eyes. Moods from Timmy's reply: happy,
 * excited (stars), curious, sad, love (hearts), laughing (> <), surprised.
 *
 * Draw cost: one pass of table lookups per grid pixel, then the eyes and
 * particles over their own small boxes.
 */

#include "timmy_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#define BIG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RAND32() esp_random()
/* Tables and particles live in PSRAM: internal RAM is needed for DMA (audio, display). */
#define PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define PSRAM_BSS
#define BIG_ALLOC(n) malloc(n)
#define RAND32() ((uint32_t)rand())
#endif

#define G 160                   /* the face's own grid, G x G */
#define C (G / 2)
#define MAX_PX 512
#define TAU 6.28318530718f
#define PI 3.14159265359f
#define DQ 8                    /* distances in 1/8 px */
#define DMAX 1024
#define EDGE (C - 2)            /* everything fades out before the canvas edge */

static uint16_t *s_dist;        /* distance from the centre, 1/8 px */
static uint8_t *s_ang;          /* angle, 0..255 clockwise from +x (screen y down) */
static uint16_t *s_fb;          /* RGB565 */
PSRAM_BSS static uint16_t s_map[MAX_PX];  /* screen px -> grid */
static int s_ox, s_oy;          /* whole-face shift (bounce, shake), grid px */

typedef struct { float r, g, b; } rgb_t;

/* Each state's hue, after Timmy's palette (timmy_pixel.c SCHEMES accents). */
static const float STATE_HUE[TIMMY_MODE_COUNT] = {
    [TIMMY_MODE_BOOT] = 222, [TIMMY_MODE_IDLE] = 265, [TIMMY_MODE_LISTENING] = 206,
    [TIMMY_MODE_THINKING] = 288, [TIMMY_MODE_SPEAKING] = 157, [TIMMY_MODE_ERROR] = 0, [TIMMY_MODE_OFF] = 246,
};
#define WORKING_HUE 35
static const float STATE_SPEED[TIMMY_MODE_COUNT] = {
    [TIMMY_MODE_BOOT] = 0.6f, [TIMMY_MODE_IDLE] = 0.3f, [TIMMY_MODE_LISTENING] = 0.6f,
    [TIMMY_MODE_THINKING] = 2.4f, [TIMMY_MODE_SPEAKING] = 0.8f, [TIMMY_MODE_ERROR] = 0.2f, [TIMMY_MODE_OFF] = 0.1f,
};
/* Moods that recolour the face; others keep the state's colour (-1). */
static const float MOOD_HUE[TIMMY_MOOD_COUNT] = {
    [TIMMY_MOOD_NONE] = -1, [TIMMY_MOOD_HAPPY] = -1, [TIMMY_MOOD_EXCITED] = 46, [TIMMY_MOOD_CURIOUS] = 178,
    [TIMMY_MOOD_SAD] = 222, [TIMMY_MOOD_LOVE] = 335, [TIMMY_MOOD_LAUGHING] = 50, [TIMMY_MOOD_SURPRISED] = 190,
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(void) { return (RAND32() & 0xffff) / 65535.0f; }
static float smooth(float cur, float want, float rate, float dt) { return cur + (want - cur) * clampf(dt * rate, 0, 1); }

static rgb_t hsl(float h, float s, float l)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = (1 - fabsf(2 * l - 1)) * s, hp = h / 60.0f, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), m = l - c / 2;
    rgb_t o = { 0, 0, 0 };
    if (hp < 1) o = (rgb_t){ c, x, 0 };
    else if (hp < 2) o = (rgb_t){ x, c, 0 };
    else if (hp < 3) o = (rgb_t){ 0, c, x };
    else if (hp < 4) o = (rgb_t){ 0, x, c };
    else if (hp < 5) o = (rgb_t){ x, 0, c };
    else o = (rgb_t){ c, 0, x };
    return (rgb_t){ (o.r + m) * 255, (o.g + m) * 255, (o.b + m) * 255 };
}

static uint32_t hex(rgb_t c)
{
    return (uint32_t)clampf(c.r, 0, 255) << 16 | (uint32_t)clampf(c.g, 0, 255) << 8 | (uint32_t)clampf(c.b, 0, 255);
}

uint32_t timmy_pixel_accent(timmy_mode_t mode)
{
    return hex(hsl(STATE_HUE[mode < TIMMY_MODE_COUNT ? mode : TIMMY_MODE_IDLE], 1.0f, 0.66f));
}

uint32_t timmy_pixel_working_accent(void)
{
    return hex(hsl(WORKING_HUE, 1.0f, 0.62f));
}

bool timmy_pixel_full_frame(void)
{
    return true;
}

static bool init_luts(void)
{
    if (s_fb) return true;
    s_dist = BIG_ALLOC(G * G * sizeof(uint16_t));
    s_ang = BIG_ALLOC(G * G);
    s_fb = BIG_ALLOC(G * G * sizeof(uint16_t));
    if (!s_dist || !s_ang || !s_fb) {
        free(s_dist), free(s_ang), free(s_fb);
        s_dist = NULL, s_ang = NULL, s_fb = NULL;
        return false;
    }
    for (int y = 0; y < G; y++) {
        for (int x = 0; x < G; x++) {
            float dx = x + 0.5f - C, dy = y + 0.5f - C;
            s_dist[y * G + x] = (uint16_t)(sqrtf(dx * dx + dy * dy) * DQ);
            float a = atan2f(dy, dx);
            s_ang[y * G + x] = (uint8_t)((int)((a < 0 ? a + TAU : a) / TAU * 256) & 255);
        }
    }
    memset(s_fb, 0, G * G * sizeof(uint16_t));
    return true;
}

void timmy_pixel_set_size(int px)
{
    if (px < 1) px = 1;
    if (px > MAX_PX) px = MAX_PX;
    for (int i = 0; i < px; i++) s_map[i] = (uint16_t)(i * G / px);
}

void timmy_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int gy = (y < MAX_PX ? s_map[y] : G - 1) - s_oy;
        if (!s_fb || gy < 0 || gy >= G) {
            memset(dst, 0, n * sizeof(uint16_t));
            continue;
        }
        const uint16_t *row = &s_fb[gy * G];
        for (int i = 0; i < n; i++) {
            int x = x0 + i;
            int gx = (x < MAX_PX ? s_map[x] : G - 1) - s_ox;
            dst[i] = (unsigned)gx < G ? row[gx] : 0;
        }
    }
}

/* ---- pixels ---- */

static inline rgb_t unpack(uint16_t c)
{
    return (rgb_t){ (float)((c >> 11) << 3), (float)(((c >> 5) & 63) << 2), (float)((c & 31) << 3) };
}

static inline uint16_t pack(float r, float g, float b)
{
    int ri = r > 255 ? 255 : r < 0 ? 0 : (int)r;
    int gi = g > 255 ? 255 : g < 0 ? 0 : (int)g;
    int bi = b > 255 ? 255 : b < 0 ? 0 : (int)b;
    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

/* Adds light (glow) at x, y. */
static inline void add_px(int x, int y, rgb_t c, float k)
{
    if ((unsigned)x >= G || (unsigned)y >= G || k <= 0.004f) return;
    uint16_t *o = &s_fb[y * G + x];
    rgb_t q = unpack(*o);
    *o = pack(q.r + c.r * k, q.g + c.g * k, q.b + c.b * k);
}

/* Paints c over x, y with coverage k. */
static inline void over_px(int x, int y, rgb_t c, float k)
{
    if ((unsigned)x >= G || (unsigned)y >= G || k <= 0.004f) return;
    uint16_t *o = &s_fb[y * G + x];
    rgb_t q = unpack(*o);
    *o = pack(q.r + (c.r - q.r) * k, q.g + (c.g - q.g) * k, q.b + (c.b - q.b) * k);
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float h = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f), 0, 1);
    float dx = wx - vx * h, dy = wy - vy * h;
    return sqrtf(dx * dx + dy * dy);
}

/* ---- eyes ---- */

typedef enum {
    EY_OVAL,      /* the usual eye; lid and slope cut it (blink, sleepy, sad) */
    EY_HAPPY,     /* ^ ^ arcs */
    EY_CONTENT,   /* u u arcs: being stroked */
    EY_CHEVRON,   /* > < laughing */
    EY_X,         /* error */
    EY_STAR,      /* excited */
    EY_HEART,     /* love */
    EY_SPIRAL,    /* dizzy */
} eye_style_t;

typedef struct {
    eye_style_t style;
    float ea, eb;       /* half width, half height */
    float open;         /* 0 shut .. 1 */
    float lid;          /* 0..1 of the eye hidden under a straight upper lid */
    float slope;        /* the lid lower at the outer corner (+) or the inner (-) */
    float spin;         /* stars and spirals turn */
    float pulse;        /* hearts and stars beat */
    rgb_t core, glow;
    bool glint;
} eye_t;

/* Signed distance (grid px, < 0 inside) from x, y to an eye centred at cx, cy. */
static float eye_sd(const eye_t *e, int side, float cx, float cy, float x, float y)
{
    float dx = x - cx, dy = y - cy;
    switch (e->style) {
    case EY_OVAL: {
        float eb = e->eb * (e->open > 0.1f ? e->open : 0.1f);
        float ea = e->ea;
        float u = dx / ea, v = dy / eb;
        float f = (sqrtf(u * u + v * v) - 1) * (ea < eb ? ea : eb);
        if (e->lid > 0.001f || e->slope != 0) {
            float lid_y = cy - eb + 2 * eb * e->lid + e->slope * eb * (dx * side / ea);
            float fl = lid_y - y;
            f = f > fl ? f : fl;
        }
        return f;
    }
    case EY_HAPPY:
    case EY_CONTENT: {
        float rad = e->ea * 1.15f, th = fmaxf(1.3f, e->ea * 0.30f);
        bool up = e->style == EY_HAPPY;
        float c0 = cy + (up ? rad * 0.35f : -rad * 0.35f);
        if (up ? y <= c0 : y >= c0) {
            float q = sqrtf(dx * dx + (y - c0) * (y - c0));
            return fabsf(q - rad) - th;
        }
        float ex = fabsf(dx) - rad, ey = y - c0;
        return sqrtf(ex * ex + ey * ey) - th;
    }
    case EY_CHEVRON: {
        float w = e->ea * 0.9f, h = e->eb * 0.55f, th = fmaxf(1.3f, e->ea * 0.28f);
        float s = (float)-side;   /* the tip points inwards */
        float ax = cx - s * w, tipx = cx + s * w * 0.6f;
        float d = fminf(seg_dist(x, y, ax, cy - h, tipx, cy), seg_dist(x, y, tipx, cy, ax, cy + h));
        return d - th;
    }
    case EY_X: {
        float w = e->ea * 0.85f, th = fmaxf(1.2f, e->ea * 0.25f);
        float d = fminf(seg_dist(x, y, cx - w, cy - w, cx + w, cy + w), seg_dist(x, y, cx - w, cy + w, cx + w, cy - w));
        return d - th;
    }
    case EY_STAR: {
        float ro = fmaxf(e->ea, e->eb) * 1.05f * (1 + 0.12f * e->pulse);
        float r = sqrtf(dx * dx + dy * dy), th = atan2f(dy, dx) + PI / 2 - e->spin;
        float R = ro * (0.42f + 0.58f * powf(fabsf(cosf(2.5f * th)), 2.2f));
        return (r - R) * 0.8f;
    }
    case EY_HEART: {
        float s = fmaxf(e->ea, e->eb) * 1.0f * (1 + 0.14f * e->pulse);
        float hx = dx / s * 1.25f, hy = -dy / s * 1.25f + 0.25f;
        float a = hx * hx + hy * hy - 1;
        float F = a * a * a - hx * hx * hy * hy * hy;
        return F * s * 0.6f;
    }
    case EY_SPIRAL: {
        float ro = fmaxf(e->ea, e->eb) * 1.0f, turns = 2.4f;
        float r = sqrtf(dx * dx + dy * dy);
        float th = atan2f(dy, dx) + e->spin * side;
        float ph = r / ro * turns - th / TAU;
        float fr = ph - floorf(ph);
        float d = fminf(fr, 1 - fr) * (ro / turns) - 1.0f;
        float out = r - ro;
        return d > out ? d : out;
    }
    }
    return 1;
}

static void draw_eye(const eye_t *e, int side, float cx, float cy, float alpha)
{
    float gl = fmaxf(2.5f, e->ea * 0.45f);
    float ext = fmaxf(e->ea, e->eb) * 1.6f + gl + 2;
    int x0 = (int)(cx - ext), x1 = (int)(cx + ext), y0 = (int)(cy - ext), y1 = (int)(cy + ext);
    bool glint = e->glint && e->style == EY_OVAL && e->open > 0.45f;
    float gx = cx - e->ea * 0.35f, gy = cy - e->eb * e->open * 0.42f, gr = e->ea * 0.30f;
    rgb_t white = { 255, 255, 255 };
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float f = eye_sd(e, side, cx, cy, px, py);
            if (f > gl) continue;
            float cov = clampf(0.5f - f, 0, 1) * alpha;
            if (f > -0.5f) {
                float q = 1 - clampf(f, 0, gl) / gl;
                add_px(x, y, e->glow, q * q * 0.55f * alpha);
            }
            if (cov > 0) {
                over_px(x, y, e->core, cov);
                if (glint) {
                    float d = sqrtf((px - gx) * (px - gx) + (py - gy) * (py - gy));
                    over_px(x, y, white, clampf(gr - d + 0.5f, 0, 1) * 0.85f * cov);
                }
            }
        }
    }
}

static void draw_soft_oval(float cx, float cy, float rx, float ry, rgb_t c, float k, bool ring)
{
    int x0 = (int)(cx - rx - 2), x1 = (int)(cx + rx + 2), y0 = (int)(cy - ry - 2), y1 = (int)(cy + ry + 2);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float u = (x + 0.5f - cx) / rx, v = (y + 0.5f - cy) / ry;
            float q = sqrtf(u * u + v * v);
            float a = ring ? clampf(1 - fabsf(q - 1) * fminf(rx, ry) / 1.3f, 0, 1) : clampf(1 - q, 0, 1) * 1.6f;
            if (a > 0) add_px(x, y, c, clampf(a, 0, 1) * k);
        }
    }
}

/* ---- particles: hearts, sparkles, stars, z's ---- */

enum { PT_HEART, PT_SPARK, PT_STAR, PT_Z, PT_DROP, PT_FLAKE, PT_CONF };

typedef struct {
    float x, y, vx, vy, life, max, size, hue;
    uint8_t kind;
} particle_t;

#define PARTICLES 40
PSRAM_BSS static particle_t s_pt[PARTICLES];

static void spawn(uint8_t kind, float x, float y, float vx, float vy, float life, float size, float hue)
{
    for (int i = 0; i < PARTICLES; i++) {
        if (s_pt[i].life <= 0) {
            s_pt[i] = (particle_t){ x, y, vx, vy, life, life, size, hue, kind };
            return;
        }
    }
}

static void draw_particle(const particle_t *p)
{
    float a = clampf(p->life / p->max, 0, 1);
    a = a > 0.75f ? (1 - a) * 4 : a / 0.75f;   /* fade in, then out */
    float s = p->size;
    int ext = (int)(s * 1.6f) + 2;
    rgb_t c = hsl(p->hue, p->kind == PT_FLAKE ? 0.25f : 1.0f,
                  p->kind == PT_SPARK || p->kind == PT_FLAKE ? 0.85f : p->kind == PT_DROP ? 0.72f : 0.66f);
    for (int y = (int)p->y - ext; y <= (int)p->y + ext; y++) {
        for (int x = (int)p->x - ext; x <= (int)p->x + ext; x++) {
            float dx = x + 0.5f - p->x, dy = y + 0.5f - p->y, k = 0;
            switch (p->kind) {
            case PT_HEART: {
                float hx = dx / s * 1.25f, hy = -dy / s * 1.25f + 0.25f;
                float q = hx * hx + hy * hy - 1;
                float F = q * q * q - hx * hx * hy * hy * hy;
                k = clampf(0.5f - F * s * 0.6f, 0, 1);
                break;
            }
            case PT_SPARK: {
                float ax = fabsf(dx), ay = fabsf(dy);
                k = clampf(1 - ax / s, 0, 1) * clampf(1 - ay * 3 / s, 0, 1) + clampf(1 - ay / s, 0, 1) * clampf(1 - ax * 3 / s, 0, 1);
                k += clampf(1 - sqrtf(dx * dx + dy * dy) / (s * 0.45f), 0, 1);
                break;
            }
            case PT_STAR: {
                float r = sqrtf(dx * dx + dy * dy), th = atan2f(dy, dx) + PI / 2;
                float R = s * (0.42f + 0.58f * powf(fabsf(cosf(2.5f * th)), 2.2f));
                k = clampf(0.5f - (r - R), 0, 1);
                break;
            }
            case PT_DROP: {   /* a raindrop: a short streak, rounder at the bottom */
                float w = s * 0.35f;
                float q = dy < 0 ? fabsf(dx) / (w * (1 + dy / (s * 1.6f))) : sqrtf(dx * dx + dy * dy) / w;
                k = dy > -s * 1.6f ? clampf(1.2f - q, 0, 1) : 0;
                break;
            }
            case PT_FLAKE: {  /* a snowflake: a soft dot with a cross */
                float ax = fabsf(dx), ay = fabsf(dy);
                k = clampf(1 - sqrtf(dx * dx + dy * dy) / (s * 0.5f), 0, 1) +
                    0.6f * (clampf(1 - ax / s, 0, 1) * clampf(1 - ay * 4 / s, 0, 1) +
                            clampf(1 - ay / s, 0, 1) * clampf(1 - ax * 4 / s, 0, 1));
                break;
            }
            case PT_CONF: {   /* confetti: a little tumbling square */
                float c = cosf(p->life * 9), sn = sinf(p->life * 9);
                float u = fabsf(dx * c + dy * sn), v = fabsf(-dx * sn + dy * c) * (1.5f + c);
                k = (u < s * 0.5f && v < s * 0.5f) ? 1 : 0;
                break;
            }
            case PT_Z: {
                float w = s * 0.7f;
                float d = fminf(fminf(seg_dist(dx, dy, -w, -w, w, -w), seg_dist(dx, dy, w, -w, -w, w)),
                                seg_dist(dx, dy, -w, w, w, w));
                k = clampf(1.1f - d, 0, 1);
                break;
            }
            }
            if (k > 0) add_px(x, y, c, clampf(k, 0, 1.4f) * a);
        }
    }
}

static void particles_update(float dt)
{
    for (int i = 0; i < PARTICLES; i++) {
        particle_t *p = &s_pt[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 1 - clampf(dt * 0.8f, 0, 1);
        if (p->kind == PT_Z) p->x += sinf(p->life * 3) * dt * 3;
    }
}

/* ---- animation state ---- */

static struct {
    float last_t, hue, sat, level, spin, hud, bright;
    float ex, ey;               /* gaze, -1..1 */
    float blink_open, blink_at, glance_at, gx, gy;
    float smile_at, smile_until;
    float quiet, heard;         /* speaking: seconds quiet, voice heard */
    float surprise;             /* seconds of surprise left */
    float yawn_at, yawn;        /* next yawn, phase 0..1 (0 = none) */
    float last_idle, happy_was;
    float spawn_acc, z_acc;
    float lid, squint, wide, round;   /* smoothed shape */
    int last_mood;
    bool init;
} s;

/* Per-frame tables for the ring pass. */
PSRAM_BSS static int16_t s_ring_out[DMAX], s_ring_in[DMAX];
PSRAM_BSS static uint8_t s_base_r[DMAX], s_base_g[DMAX], s_base_b[DMAX];
PSRAM_BSS static uint8_t s_hud_k[DMAX], s_win[DMAX];
PSRAM_BSS static uint8_t s_fr_k[48];
PSRAM_BSS static uint8_t s_sw_r[256], s_sw_g[256], s_sw_b[256];
PSRAM_BSS static uint8_t s_fr_r[256], s_fr_g[256], s_fr_b[256];
PSRAM_BSS static uint8_t s_hud_m[256];
PSRAM_BSS static int16_t s_roff[256];
PSRAM_BSS static uint8_t s_arc_k[DMAX], s_arc_m[256];   /* countdown ring: profile by distance, mask by angle */

void timmy_pixel_render(const timmy_pose_t *p)
{
    if (!init_luts()) return;
    float t = p->t;
    float gap = s.last_t > 0 ? p->t - s.last_t : 0;
    float dt = s.last_t > 0 ? clampf(gap, 0, 0.2f) : 0.04f;
    s.last_t = t;
    timmy_mode_t mode = p->mode < TIMMY_MODE_COUNT ? p->mode : TIMMY_MODE_IDLE;
    bool working = mode == TIMMY_MODE_THINKING && p->working > 0.5f;
    float happy = clampf(p->happy, 0, 1), pet = clampf(p->pet, 0, 1), dizzy = clampf(p->dizzy, 0, 1);
    bool offline = p->offline > 0.5f && mode == TIMMY_MODE_IDLE;
    int mood = (p->mood > 0 && p->mood < TIMMY_MOOD_COUNT &&
                (mode == TIMMY_MODE_SPEAKING || (mode == TIMMY_MODE_IDLE && p->mood_t < 4.0f))) ? p->mood : 0;
    /* A look from the assistant's skills shows while idle (and through a reply it came with). */
    int look = p->look > 0 && p->look < TIMMY_LOOK_COUNT && mode != TIMMY_MODE_ERROR && mode != TIMMY_MODE_BOOT &&
               mode != TIMMY_MODE_OFF && mode != TIMMY_MODE_LISTENING ? p->look : TIMMY_LOOK_NONE;
    float lt = p->look_t;
    bool asleep = look == TIMMY_LOOK_ASLEEP;
    float drowsy = mode == TIMMY_MODE_IDLE && !offline && !mood && p->notice < 0.5f && p->waiting < 0.5f && !look
                   ? clampf((p->idle_s - 45) / 20, 0, 1) : 0;
    if (look == TIMMY_LOOK_SLEEPY) drowsy = 0.8f;

    if (!s.init) {
        s.hue = STATE_HUE[mode];
        s.sat = 1;
        s.blink_open = 1;
        s.blink_at = t + 2.5f;
        s.glance_at = t + 3;
        s.smile_at = t + 8 + frand() * 8;
        s.yawn_at = t + 4;
        s.init = true;
    }

    /* Waking up, or a tap that ends a doze: a surprised look. */
    if (((gap > 2.0f && mode == TIMMY_MODE_IDLE) || (s.last_idle > 45 && p->idle_s < 1.0f && mode == TIMMY_MODE_IDLE))
        && look != TIMMY_LOOK_POUT && !asleep) {
        s.surprise = 1.1f;
    }
    if (look == TIMMY_LOOK_SURPRISED && lt < 0.2f) s.surprise = 1.0f;
    if (mood == TIMMY_MOOD_SURPRISED && s.last_mood != TIMMY_MOOD_SURPRISED) s.surprise = 1.0f;
    s.last_idle = p->idle_s;
    s.surprise = fmaxf(0, s.surprise - dt);

    /* ---- colour ---- */
    float want_hue = working ? WORKING_HUE : STATE_HUE[mode], want_sat = 1;
    if (mood && MOOD_HUE[mood] >= 0) want_hue = MOOD_HUE[mood];
    if (mood == TIMMY_MOOD_SAD) want_sat = 0.45f;
    if (look && p->look_hue >= 0) want_hue = p->look_hue;
    if (asleep) want_sat = 0.55f;
    if (offline) want_hue = 215, want_sat = 0.35f;
    if (pet > 0.3f) want_hue = 330;
    float dh = fmodf(want_hue - s.hue + 540.0f, 360.0f) - 180.0f;   /* the short way round */
    s.hue += dh * clampf(dt * 3, 0, 1);
    s.sat = smooth(s.sat, want_sat, 3, dt);
    float hue = s.hue, sat = s.sat;

    /* ---- level and motion ---- */
    float target;
    switch (mode) {
    case TIMMY_MODE_LISTENING: target = clampf(p->level * 1.6f, 0, 1); break;
    case TIMMY_MODE_SPEAKING: target = clampf(p->level * 1.4f, 0, 1); break;
    case TIMMY_MODE_THINKING: target = 0.30f + 0.12f * sinf(t / 0.12f); break;
    default: target = 0.08f + 0.05f * sinf(t / 0.9f); break;
    }
    s.level = smooth(s.level, target, 10, dt);
    float speed = STATE_SPEED[mode] * (offline ? 0.4f : 1) * (mood == TIMMY_MOOD_EXCITED ? 2.0f : 1) *
                  (1 - drowsy * 0.6f) * (asleep ? 0.2f : look == TIMMY_LOOK_FOCUS ? 0.5f : 1) + dizzy * 3;
    s.spin = fmodf(s.spin + dt * speed * 1.3f, 5 * TAU);   /* 5 turns: the comet's 1.6x turns whole too */
    s.hud = fmodf(s.hud + dt * speed, TAU);
    float level = s.level;

    /* ---- eyes: blinks, glances, smiles ---- */
    float since = t - s.blink_at;
    if (since >= 0) {
        float close = 0.07f * (1 + drowsy * 2), reopen = 0.09f * (1 + drowsy * 3);
        s.blink_open = since < close ? 1 - since / close : since < close + reopen ? (since - close) / reopen : 1;
        if (since >= close + reopen) {
            s.blink_open = 1;
            s.blink_at = t + (frand() < 0.2f ? 0.22f : 2.5f + frand() * 3.5f) * (1 + drowsy);
        }
    }
    if (t > s.glance_at) {
        if (frand() < 0.45f) s.gx = 0, s.gy = 0;
        else s.gx = (frand() * 2 - 1) * 0.8f, s.gy = (frand() * 2 - 1) * 0.35f;
        s.glance_at = t + 1.2f + frand() * 3;
    }
    if (mode == TIMMY_MODE_IDLE && t > s.smile_at) {
        s.smile_until = t + 1.0f;
        s.smile_at = t + 9 + frand() * 9;
    }
    if (mode == TIMMY_MODE_SPEAKING) {
        if (p->level > 0.15f) s.heard = 1;
        s.quiet = p->level < 0.05f ? s.quiet + dt : 0;
    } else {
        s.heard = 0, s.quiet = 0;
    }
    /* Yawns while drowsy. */
    if (drowsy > 0.5f && s.yawn <= 0 && t > s.yawn_at) {
        s.yawn = 0.001f;
        s.yawn_at = t + 10 + frand() * 6;
    }
    if (s.yawn > 0) {
        s.yawn += dt / 2.2f;
        if (s.yawn >= 1) s.yawn = 0;
    }
    float yawn = s.yawn > 0 ? sinf(s.yawn * PI) : 0;

    /* Gaze: a finger, then the tilt, then the state. */
    float tx = s.gx, ty = s.gy;
    if (working) {
        tx = 0.45f * sinf(p->mode_t * 2.1f), ty = 0.55f;
    } else if (mode == TIMMY_MODE_THINKING) {
        tx = 0.5f * sinf(t / 0.7f) + 0.15f, ty = -0.65f;
    } else if (mode == TIMMY_MODE_LISTENING) {
        tx = 0, ty = 0;
    } else if (mode == TIMMY_MODE_SPEAKING) {
        tx *= 0.5f, ty = -0.2f * level * fabsf(sinf(t / 0.11f));
    } else if (mood == TIMMY_MOOD_CURIOUS) {
        tx = 0.5f, ty = -0.35f;
    }
    if (drowsy > 0) ty += drowsy * 0.3f;
    if (offline) ty = 0.35f, tx *= 0.3f;
    if (p->notice > 0.5f && mode == TIMMY_MODE_IDLE) tx *= 0.3f, ty = 0.55f;   /* reading the answer below */
    switch (look) {
    case TIMMY_LOOK_POUT: tx = 0.75f, ty = 0.15f; break;                 /* won't look at you */
    case TIMMY_LOOK_FOCUS: case TIMMY_LOOK_ASLEEP: tx *= 0.2f, ty = 0.1f; break;
    case TIMMY_LOOK_DROOP: ty = 0.4f, tx *= 0.4f; break;
    case TIMMY_LOOK_SQUINT: ty = -0.25f; break;                          /* up at the sun */
    case TIMMY_LOOK_GLOW: tx = 0, ty = 0; break;                         /* right at you */
    default: break;
    }
    if (fabsf(p->tilt_x) > 0.08f || fabsf(p->tilt_y) > 0.08f) {
        tx = clampf(tx * 0.3f + p->tilt_x * 1.2f, -1, 1), ty = clampf(ty * 0.3f + p->tilt_y * 1.2f, -1, 1);
    }
    if (p->touch > 0.5f) tx = clampf(p->touch_x * 1.3f, -1, 1), ty = clampf(p->touch_y * 1.3f, -1, 1);
    if (dizzy > 0.1f) tx = 0, ty = 0;
    float gaze_rate = p->touch > 0.5f ? 18 : 12;
    s.ex = smooth(s.ex, tx, gaze_rate, dt);
    s.ey = smooth(s.ey, ty, gaze_rate, dt);

    /* ---- shape of the ring ---- */
    float breathe = 1 + 0.02f * sinf(t / 1.1f) * (1 + drowsy);
    float R = G * 0.31f * breathe * (1 + level * 0.14f + yawn * 0.06f);
    float boot_reveal = 1, boot_eyes = 1;
    if (mode == TIMMY_MODE_BOOT) {
        boot_reveal = clampf(p->mode_t / 0.9f, 0, 1);
        float k = clampf((p->mode_t - 0.9f) / 0.35f, 0, 1);
        boot_eyes = k < 1 ? k * (1 + 0.35f * sinf(k * PI)) : 1;
    }
    float fade = mode == TIMMY_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    bool notice = p->notice > 0.5f && mode == TIMMY_MODE_IDLE;
    float bright = 1 + (s.surprise > 0 ? 0.35f * s.surprise : 0) - drowsy * 0.25f - (offline ? 0.25f : 0)
                   + (notice ? 0.30f * (0.5f + 0.5f * sinf(t * 2.2f)) : 0)
                   + (look == TIMMY_LOOK_GLOW ? 0.45f * (0.5f + 0.5f * sinf(t * 6)) : 0)
                   + (look == TIMMY_LOOK_SQUINT ? 0.2f : 0) - (asleep ? 0.5f : 0);
    s.bright = smooth(s.bright, bright, 6, dt);
    float ring_w = R * (0.09f + level * 0.06f);
    float rain = clampf(happy * 1.2f, 0, 1);

    /* Ring colour around the circle, its brightness per angle (boot draws it
     * round from the top, thinking runs a comet), and its ripples. */
    const float stops[5] = { 60, 25, -15, 95, 60 };
    float variety = mode == TIMMY_MODE_ERROR ? 0.25f : 1;   /* error stays red */
    int head = (int)(fmodf(s.spin * 1.6f / TAU, 1.0f) * 256) & 255;
    for (int i = 0; i < 256; i++) {
        float u = i / 256.0f - s.spin / TAU;
        u = (u - floorf(u)) * 4;
        if (u >= 4) u = 0;
        int k = (int)u;
        float f = u - k;
        float h = hue + (stops[k] + (stops[k + 1] - stops[k]) * f) * variety + rain * i * 360.0f / 256;
        rgb_t c = hsl(h, sat, 0.62f);
        float m = 1;
        if (mode == TIMMY_MODE_BOOT) {
            int from_top = (i - 192) & 255;
            float rev = boot_reveal * 256;
            m = from_top < rev ? 1 + (from_top > rev - 16 ? 0.8f : 0) : 0;
        } else if (mode == TIMMY_MODE_THINKING && !working) {
            float behind = ((head - i) & 255) / 256.0f;
            m = 0.30f + 0.95f * (1 - behind) * (1 - behind);
        }
        m *= fade * s.bright;
        s_sw_r[i] = (uint8_t)clampf(c.r * m, 0, 255);
        s_sw_g[i] = (uint8_t)clampf(c.g * m, 0, 255);
        s_sw_b[i] = (uint8_t)clampf(c.b * m, 0, 255);
        /* chromatic fringes: red at the sides, green-cyan at the bottom */
        float ca = cosf(i * TAU / 256), sa = sinf(i * TAU / 256);
        float wr = powf(ca * ca, 3) * 0.45f, wc = sa > 0 ? powf(sa, 6) * 0.30f : 0;
        float fr = 255 * wr + 40 * wc, fg = 40 * wr + 255 * wc, fb = 90 * wr + 200 * wc;
        s_fr_r[i] = (uint8_t)(fr * m), s_fr_g[i] = (uint8_t)(fg * m), s_fr_b[i] = (uint8_t)(fb * m);
        /* ripples while listening, a wobble while dizzy, a quiver laughing */
        float a = i * TAU / 256;
        float off = level * (mode == TIMMY_MODE_LISTENING ? R * 0.07f * sinf(a * 6 + t * 7) : 0)
                    + dizzy * R * 0.06f * sinf(a * 2 + t * 7)
                    + (mood == TIMMY_MOOD_LAUGHING ? R * 0.02f * sinf(a * 8 + t * 20) : 0);
        s_roff[i] = (int16_t)(off * DQ);
        int seg = (i - ((int)(s.hud / TAU * 256) & 255)) & 255;
        s_hud_m[i] = (seg % 85) < 47;
    }

    rgb_t halo = hsl(hue + 20, sat, 0.60f);
    float halo_a = (0.16f + level * 0.3f) * fade * s.bright;
    rgb_t gloss = hsl(hue + 40, 0.8f * sat, 0.80f);
    rgb_t hudc = hsl(hue, sat, 0.65f);
    float boost = (1 + level * 0.4f) * fade * s.bright;
    float hud_r = R * 1.40f;
    for (int i = 0; i < DMAX; i++) {
        float d = (float)i / DQ;
        float core = d < ring_w * 0.35f ? 1.0f : expf(-(d - ring_w * 0.35f) / (ring_w * 0.25f + 0.5f));
        float soft = expf(-d / (R * 0.10f)) * 0.55f + expf(-d / (R * 0.28f)) * 0.25f;
        float k = clampf((core * 0.9f + soft) * boost, 0, 1.6f);
        s_ring_out[i] = (int16_t)(k * 256);
        s_ring_in[i] = (int16_t)(k * (d > ring_w * 0.5f ? 128 : 256));
        /* by distance from the centre: halo outside the ring, gloss inside it */
        float br = 0, bg = 0, bb = 0;
        if (d > R) {
            float f = 1 - (d - R) / (EDGE - R);
            if (f > 0) { float q = halo_a * f * f; br += halo.r * q, bg += halo.g * q, bb += halo.b * q; }
        } else if (d > R * 0.75f) {
            float q = (d - R * 0.75f) / (R * 0.23f) * 0.10f * fade;
            br += gloss.r * q, bg += gloss.g * q, bb += gloss.b * q;
        }
        s_base_r[i] = (uint8_t)clampf(br, 0, 255), s_base_g[i] = (uint8_t)clampf(bg, 0, 255);
        s_base_b[i] = (uint8_t)clampf(bb, 0, 255);
        float hd = fabsf(d - hud_r);
        s_hud_k[i] = (uint8_t)(hd < 1.25f ? 0.45f * (1 - hd / 1.25f) * 255 * fade * s.bright : 0);
        s_win[i] = (uint8_t)(clampf((EDGE - d) / 12.0f, 0, 1) * 255);
    }
    for (int i = 0; i < 48; i++) s_fr_k[i] = (uint8_t)((i < 40 ? 1 - i / 40.0f : 0) * 255);
    /* Countdown ring (a skill's timer): an arc from the top, clockwise, shrinking as time runs out. */
    bool arc = p->ring > 0 && look;
    if (arc) {
        float arc_r = R * 1.20f, left = clampf(p->ring, 0, 1);
        for (int i = 0; i < DMAX; i++) {
            float hd = fabsf(i / (float)DQ - arc_r);
            s_arc_k[i] = (uint8_t)(hd < 2.2f ? (1 - hd / 2.2f) * 230 * fade : 0);
        }
        for (int i = 0; i < 256; i++) s_arc_m[i] = (((i - 192) & 255) < left * 256) ? 1 : 0;
    }
    rgb_t arcc = hsl(hue + 10, 1, 0.70f);
    int arr = (int)arcc.r, arg = (int)arcc.g, arb = (int)arcc.b;

    /* ---- the ring pass ---- */
    int rq = (int)(R * DQ), fo = (int)(R * 0.025f * (1 + level) * DQ);
    int dcut = EDGE * DQ, dwin = (EDGE - 12) * DQ;
    int hr = (int)hudc.r, hg = (int)hudc.g, hb = (int)hudc.b;
    for (int y = 0; y < G; y++) {
        uint16_t *out = &s_fb[y * G];
        const uint16_t *drow = &s_dist[y * G];
        const uint8_t *arow = &s_ang[y * G];
        for (int x = 0; x < G; x++) {
            int d = drow[x];
            if (d >= dcut) { out[x] = 0; continue; }
            int a = arow[x];
            int dr = d - rq - s_roff[a];
            int adr = dr < 0 ? -dr : dr;
            int k = adr < DMAX ? (dr < 0 ? s_ring_in[adr] : s_ring_out[adr]) : 0;
            int r = ((s_sw_r[a] * k) >> 8) + s_base_r[d];
            int g = ((s_sw_g[a] * k) >> 8) + s_base_g[d];
            int b = ((s_sw_b[a] * k) >> 8) + s_base_b[d];
            int fd = dr - fo;
            fd = fd < 0 ? -fd : fd;
            if (fd < 40) {
                int q = s_fr_k[fd];
                r += (s_fr_r[a] * q) >> 8, g += (s_fr_g[a] * q) >> 8, b += (s_fr_b[a] * q) >> 8;
            }
            int hk = s_hud_k[d];
            if (hk && s_hud_m[a]) r += (hr * hk) >> 8, g += (hg * hk) >> 8, b += (hb * hk) >> 8;
            if (arc && s_arc_k[d] && s_arc_m[a]) {
                int ak = s_arc_k[d];
                r += (arr * ak) >> 8, g += (arg * ak) >> 8, b += (arb * ak) >> 8;
            }
            if (d > dwin) {
                int w = s_win[d];
                r = (r * w) >> 8, g = (g * w) >> 8, b = (b * w) >> 8;
            }
            r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
            out[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }

    /* Working: a dot orbits on the HUD ring. */
    if (working) {
        for (int j = 0; j < 6; j++) {
            float a = t * 3.2f - j * 0.12f;
            draw_soft_oval(C + cosf(a) * hud_r, C + sinf(a) * hud_r, 2.6f - j * 0.3f, 2.6f - j * 0.3f,
                           hsl(hue, 1, 0.75f), (1 - j / 6.0f) * fade, false);
        }
    }

    /* ---- the eyes ---- */
    eye_t e = {
        .style = EY_OVAL,
        .ea = R * 0.17f,
        .eb = R * 0.28f,
        .open = s.blink_open,
        .core = hsl(hue, 0.9f * sat, 0.84f),
        .glow = hsl(hue, sat, 0.70f),
        .glint = true,
        .spin = t * 1.5f,
        .pulse = 0,
    };
    float want_lid = 0, want_slope = 0, want_wide = 1, want_round = 0;
    bool smile = t < s.smile_until;
    switch (mode) {
    case TIMMY_MODE_LISTENING: want_wide = 1.15f + level * 0.30f; break;
    case TIMMY_MODE_THINKING: want_wide = working ? 0.9f : 0.88f; want_lid = working ? 0.22f : 0.12f; break;
    case TIMMY_MODE_SPEAKING:
        e.eb *= 1 + 0.20f * level;
        e.ea *= 1 - 0.06f * level;
        if (s.heard > 0 && s.quiet > (mood == TIMMY_MOOD_HAPPY ? 0.25f : 0.5f)) smile = true;
        break;
    case TIMMY_MODE_ERROR: e.style = EY_X; break;
    case TIMMY_MODE_BOOT: e.open = boot_eyes; break;
    case TIMMY_MODE_OFF: e.open = clampf(1.0f - p->mode_t / 1.0f, 0, 1); break;
    default: break;
    }
    switch (look) {
    case TIMMY_LOOK_HAPPY: smile = true; break;
    case TIMMY_LOOK_CHEER: smile = true; break;
    case TIMMY_LOOK_SQUINT: want_lid = 0.48f, want_slope = 0.15f; break;
    case TIMMY_LOOK_DROOP: want_lid = 0.30f, want_slope = 0.5f; break;
    case TIMMY_LOOK_SHIVER: want_lid = 0.25f, want_wide = 0.9f; break;
    case TIMMY_LOOK_CALM: want_lid = 0.18f; break;
    case TIMMY_LOOK_GLOW: want_wide = 1.15f; break;
    case TIMMY_LOOK_FOCUS: want_lid = 0.22f, want_wide = 0.95f; break;
    case TIMMY_LOOK_SLEEPY: want_lid = 0.55f; break;
    case TIMMY_LOOK_ASLEEP: e.open = 0.04f; break;
    case TIMMY_LOOK_POUT: want_lid = 0.32f, want_slope = -0.25f, want_wide = 0.9f; break;
    default: break;
    }
    if (offline) want_lid = 0.30f, want_slope = 0.55f;
    if (drowsy > 0) want_lid = fmaxf(want_lid, drowsy * 0.50f);
    if (yawn > 0) e.open = fminf(e.open, 1 - yawn * 0.85f);
    if (s.surprise > 0) want_wide = 1.15f, want_round = 1, want_lid = 0;

    /* Moods pick an eye; reactions (a finger's touch) win over moods. */
    float curious_l = 1, curious_r = 1;
    if (mode != TIMMY_MODE_ERROR && mode != TIMMY_MODE_BOOT && mode != TIMMY_MODE_OFF) {
        switch (mood) {
        case TIMMY_MOOD_HAPPY: if (mode != TIMMY_MODE_SPEAKING) smile = true; break;
        case TIMMY_MOOD_EXCITED: e.style = EY_STAR; e.pulse = level + 0.3f * sinf(t * 8); break;
        case TIMMY_MOOD_CURIOUS: curious_l = 1.18f, curious_r = 0.85f; want_lid = 0.0f; break;
        case TIMMY_MOOD_SAD: want_lid = 0.32f, want_slope = 0.6f; break;
        case TIMMY_MOOD_LOVE: e.style = EY_HEART; e.pulse = fmaxf(0, sinf(t * 7)); break;
        case TIMMY_MOOD_LAUGHING: e.style = EY_CHEVRON; break;
        case TIMMY_MOOD_SURPRISED: want_wide = 1.15f, want_round = 1; break;
        default: break;
        }
        if (smile && e.style == EY_OVAL) e.style = EY_HAPPY;
        if (happy > 0.2f) e.style = EY_HAPPY;
        if (pet > 0.15f) e.style = EY_CONTENT;
        if (dizzy > 0.15f) e.style = EY_SPIRAL;
    }
    s.lid = smooth(s.lid, want_lid, 8, dt);
    s.squint = smooth(s.squint, want_slope, 8, dt);
    s.wide = smooth(s.wide, want_wide, 10, dt);
    s.round = smooth(s.round, want_round, 10, dt);
    e.lid = s.lid;
    e.slope = s.squint;
    e.eb *= s.wide;
    e.ea *= 1 + (s.wide - 1) * 0.5f;
    e.ea += (e.eb * 0.78f - e.ea) * s.round;   /* surprised: rounder */
    if (e.style == EY_OVAL && e.open < 0.15f && mode != TIMMY_MODE_OFF && mode != TIMMY_MODE_BOOT) e.glint = false;

    /* Bounce for joy, a shake for errors, a sway when dizzy, a slide with the tilt. */
    float ox = 0, oy = 0;
    if (happy > 0) oy -= fabsf(sinf(t * 9.0f)) * 4.0f * happy;
    if (mood == TIMMY_MOOD_LAUGHING) oy += sinf(t * 24) * 1.4f;
    if (mood == TIMMY_MOOD_EXCITED) oy -= fabsf(sinf(t * 6)) * 1.5f;
    if (mode == TIMMY_MODE_ERROR && p->mode_t < 0.6f) ox += sinf(t * 40) * 3;
    if (dizzy > 0) ox += cosf(t * 5) * 2.5f * dizzy, oy += sinf(t * 5) * 2.0f * dizzy;
    if (pet > 0) ox += sinf(t * 3) * 1.2f * pet;
    if (look == TIMMY_LOOK_CHEER) oy -= fabsf(sinf(t * 9.0f)) * 3.5f;
    if (look == TIMMY_LOOK_SHIVER) ox += sinf(t * 55) * 1.1f;
    if (look == TIMMY_LOOK_POUT) oy += 1;
    ox += clampf(p->tilt_x, -1, 1) * 3, oy += clampf(p->tilt_y, -1, 1) * 3;
    s_ox = (int)lroundf(ox), s_oy = (int)lroundf(oy);

    float spread = R * 0.33f;
    float ecx[2] = { C - spread + s.ex * R * 0.28f, C + spread + s.ex * R * 0.28f };
    float ecy = C - R * 0.05f + s.ey * R * 0.26f;
    if (e.style == EY_HAPPY || e.style == EY_CONTENT) ecy -= R * 0.03f;
    float alpha = fade * (mode == TIMMY_MODE_BOOT ? clampf(boot_eyes * 3, 0, 1) : 1);
    if (alpha > 0.01f && !(mode == TIMMY_MODE_BOOT && boot_eyes <= 0)) {
        for (int i = 0; i < 2; i++) {
            eye_t ei = e;
            float k = i ? curious_r : curious_l;
            ei.ea *= k, ei.eb *= k;
            if (k < 1) ei.lid = fmaxf(ei.lid, 0.25f);
            draw_eye(&ei, i ? 1 : -1, ecx[i], ecy, alpha);
        }
    }

    /* Blush: while stroked, happy, loved or laughing. */
    float blush = fmaxf(pet, fmaxf(happy * 0.7f, (mood == TIMMY_MOOD_LOVE || mood == TIMMY_MOOD_LAUGHING) ? 0.6f : 0));
    if (blush > 0.02f) {
        rgb_t pink = hsl(345, 1, 0.62f);
        for (int i = 0; i < 2; i++) {
            float side = i ? 1 : -1;
            draw_soft_oval(ecx[i] + side * e.ea * 0.9f, ecy + R * 0.28f, e.ea * 1.0f, R * 0.07f, pink, blush * 0.55f * fade, false);
        }
    }
    /* A yawn, or surprise: a little "o". */
    float o = fmaxf(yawn, s.surprise > 0 ? 0.55f * clampf(s.surprise * 2, 0, 1) : 0);
    if (o > 0.05f) {
        draw_soft_oval(C + s.ex * R * 0.2f, ecy + R * 0.42f, R * 0.07f * (0.5f + o), R * 0.09f * (0.5f + o),
                       e.core, 0.9f * o * fade, true);
    }
    /* Questions saved offline: three dots under the eyes, one after another. */
    if (p->waiting > 0.5f && mode == TIMMY_MODE_IDLE) {
        for (int j = 0; j < 3; j++) {
            float ph = fmodf(t * 1.6f - j * 0.33f, 1.0f);
            float k = ph < 0.5f ? ph * 2 : (1 - ph) * 2;
            draw_soft_oval(C + (j - 1) * R * 0.14f, ecy + R * 0.48f, R * 0.035f + k * 0.6f, R * 0.035f + k * 0.6f,
                           e.core, (0.35f + 0.65f * k) * fade, false);
        }
    }
    /* Dizzy: little stars circling over the eyes. */
    if (dizzy > 0.1f) {
        for (int j = 0; j < 3; j++) {
            float a = t * 4 + j * TAU / 3;
            particle_t st = { C + cosf(a) * R * 0.45f, ecy - R * 0.50f + sinf(a) * R * 0.12f, 0, 0,
                              0.5f * dizzy, 1, 3.2f, 50, PT_STAR };
            draw_particle(&st);
        }
    }

    /* ---- particles ---- */
    if (happy > 0.5f && s.happy_was <= 0.5f) {
        for (int j = 0; j < 9; j++) {
            float a = -PI / 2 + (frand() - 0.5f) * 2.4f;
            spawn(j % 3 ? PT_HEART : PT_SPARK, C + cosf(a) * R, C + sinf(a) * R, cosf(a) * (14 + frand() * 10),
                  sinf(a) * (14 + frand() * 10) - 10, 1.2f + frand() * 0.5f, 2.6f + frand() * 1.4f,
                  j % 3 ? 345 : hue + 40);
        }
    }
    s.happy_was = happy;
    s.spawn_acc += dt;
    float every = 0;
    uint8_t kind = PT_SPARK;
    float ph = hue + 30;
    if (mood == TIMMY_MOOD_LOVE) every = 0.35f, kind = PT_HEART, ph = 345;
    else if (mood == TIMMY_MOOD_EXCITED) every = 0.22f, kind = PT_STAR, ph = 48;
    else if (mood == TIMMY_MOOD_LAUGHING || mood == TIMMY_MOOD_HAPPY) every = 0.45f, kind = PT_SPARK;
    else if (pet > 0.3f) every = 0.55f, kind = PT_HEART, ph = 345;
    if (every > 0 && s.spawn_acc > every) {
        s.spawn_acc = 0;
        float a = frand() * TAU;
        float rr = R * (1.05f + frand() * 0.2f);
        spawn(kind, C + cosf(a) * rr, C + sinf(a) * rr, cosf(a) * 6, sinf(a) * 6 - 12, 1.3f, 2.4f + frand() * 1.4f, ph);
    }
    /* A look's particles: sun sparkles, rain, snow, hearts, stars, confetti, z's, water drops. */
    static float look_acc;
    static int last_look;
    int parts = look ? p->particles : 0;
    if (look == TIMMY_LOOK_CHEER && last_look != TIMMY_LOOK_CHEER) {
        for (int j = 0; j < 16; j++) {   /* a burst of confetti */
            float a = -PI / 2 + (frand() - 0.5f) * 2.8f, v = 20 + frand() * 18;
            spawn(PT_CONF, C + cosf(a) * R * 0.9f, C + sinf(a) * R * 0.9f, cosf(a) * v, sinf(a) * v - 8,
                  1.4f + frand() * 0.6f, 2.2f + frand(), frand() * 360);
        }
    }
    last_look = look;
    look_acc += dt;
    float rate = parts == TIMMY_PARTICLES_RAIN ? 0.07f : parts == TIMMY_PARTICLES_SNOW ? 0.16f
               : parts == TIMMY_PARTICLES_Z ? 1.6f : parts == TIMMY_PARTICLES_CONFETTI ? 0.15f : 0.3f;
    if (parts && look_acc > rate) {
        look_acc = 0;
        float x = C + (frand() * 2 - 1) * R * 0.95f, a = frand() * TAU;
        switch (parts) {
        case TIMMY_PARTICLES_SUN:
            spawn(PT_SPARK, C + cosf(a) * R * 1.15f, C + sinf(a) * R * 1.15f, cosf(a) * 4, sinf(a) * 4, 1.0f,
                  2.4f + frand(), 48);
            break;
        case TIMMY_PARTICLES_RAIN:
            spawn(PT_DROP, x, C - R * 1.1f, -3, 55 + frand() * 15, 2.0f * R / 60, 2.6f, 205);
            break;
        case TIMMY_PARTICLES_SNOW:
            spawn(PT_FLAKE, x, C - R * 1.1f, (frand() - 0.5f) * 8, 13 + frand() * 6, 2.0f * R / 15, 2.0f + frand(), 200);
            break;
        case TIMMY_PARTICLES_HEARTS:
            spawn(PT_HEART, x, C + R * 0.6f, 0, -14, 1.6f, 2.6f + frand(), 345);
            break;
        case TIMMY_PARTICLES_STARS:
            spawn(PT_STAR, C + cosf(a) * R * 1.1f, C + sinf(a) * R * 1.1f, 0, -3, 1.2f, 2.6f + frand(), 50);
            break;
        case TIMMY_PARTICLES_CONFETTI:
            spawn(PT_CONF, x, C - R * 1.1f, (frand() - 0.5f) * 10, 18, 2.2f, 2.2f, frand() * 360);
            break;
        case TIMMY_PARTICLES_Z:
            spawn(PT_Z, C + R * 0.55f, C - R * 0.55f, 5, -9, 2.4f, 2.6f + frand(), hue);
            break;
        case TIMMY_PARTICLES_DROPS:
            spawn(PT_DROP, C + (frand() - 0.5f) * R * 0.6f, C - R * 0.9f, 0, 30, 1.4f, 2.4f, 200);
            break;
        }
    }
    if (drowsy > 0.9f && p->idle_s > 70) {
        s.z_acc += dt;
        if (s.z_acc > 1.6f) {
            s.z_acc = 0;
            spawn(PT_Z, C + R * 0.55f, C - R * 0.55f, 5, -9, 2.4f, 2.6f + frand(), hue);
        }
    }
    particles_update(dt);
    for (int i = 0; i < PARTICLES; i++) {
        if (s_pt[i].life > 0) draw_particle(&s_pt[i]);
    }
    s.last_mood = mood;
}

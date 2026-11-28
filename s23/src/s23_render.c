/*
 * s23_render.c -- System 23's PICTURE, our own code: the C435's render list (src/s23_c435.c) drawn as the hardware draws it, and the frame
 * composed as MAME's namcos23.cpp screen_update describes it --
 *   the C404 background colour (faded with fade flag bit 0) -> the C361 text layer's low-priority pixels -> the polygons -> the text pixels
 *   the polygons cover (MAME's priority map: text marks 4, a polygon pixel turns a mark into 6) -- each layer enabled by the C404's layer flags
 *   (bit 2 text, bit 0 polygons).
 * THE MODELS: the point ROM (tss1pt*: 32-bit words, word = high chip << 16 | low chip) starts with a table of model addresses; a model is a run
 * of polygons -- type word (vertex count, light mode, colour mode, texture base, "last" bit 16), header word h (colour, z-sort mode, back-face
 * test, blend, stencil), an optional z-shift word, a light word (or one packed normal a vertex), then per vertex three s24 coordinates with
 * the texture u/v packed into their top bits. Each vertex goes through the entry's matrix, scale and translation (blended with a second model
 * by the blend factor), is clipped at the near plane, projected (x / z * the viewport's focal length, about its centre) and the polygon is
 * z-sorted (min / max / mid depth, plus the z-shift and the absolute priority) and drawn far to near, perspective-correct: texture tiles
 * from the tile map (tss1ccrl/h: tile number, flips, swap) into the 16x16 8-bit tiles (tss1cg*), the palette bank `colour`, Gouraud shade
 * (0..63 = x n / 64), the C404's polygon colour fade, screen fade, alpha and 50% blend.
 * Immediate polygons (state 0x0000 / 0x0002 / 0x000A) are drawn the same way from their own vertices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <pthread.h>
#include <stddef.h>
#include "s23_c435.h"
#include "s23_frame.h"
#include "s23_build.h"
#include "eng_display.h"
#include "s23_game.h"
#include "s23_host.h"

uint8_t *s23_vram_ptr(uint32_t p);
void s23_text_scroll(uint16_t linexscroll[1024], uint16_t *yscroll);
uint16_t *s23_c412_sram(void);

static uint32_t *ptrom; static uint32_t ptrom_limit;
static uint16_t *tmlrom; static uint8_t *tmhrom, *texrom, *sprrom;
static uint32_t *tm_decoded; static uint8_t *tattr_decoded;
static const uint32_t tileid_mask = 0x1FFF00, tile_mask = 0x1FFFF;
static int roms_ok;
unsigned long s23_render_direct, s23_render_frames, s23_render_entries, s23_render_models, s23_render_immediate, s23_render_polys;

static int load(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "[3D] cannot read %s\n", p); return 0; }
    const size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[3D] %s: %zu of %zu bytes\n", p, got, n); return 0; }
    return 1;
}
/* a point ROM word for the C417's read port (src/s23_chips.c): 1 = inside the ROM */
int s23_ptrom_word(uint32_t a, uint32_t *w) { if (!ptrom || a >= ptrom_limit) return 0; *w = ptrom[a]; return 1; }
void s23_render_init(const char *dir)
{
    ptrom_limit = 0x800000; ptrom = calloc(ptrom_limit + 256, 4);   /* + a pad: a polygon's vertex words are read after the per-polygon limit check */
    uint8_t *h = malloc(0x400000), *l = malloc(0x400000);
    int ok = 1;
    for (int c = 0; c < s23_game.npt; c++) {         /* the game's point ROM chip pairs (include/s23_game.h): 3 on Time Crisis 2, 4 on Crisis Zone */
        /* MAME ROM_LOAD32_WORD_SWAP into a big-endian 32-bit region: the "h" chip's 16-bit words are the high halves, byte-swapped */
        ok &= load(dir, s23_game.pt_rom[c][0], h, 0x400000) & load(dir, s23_game.pt_rom[c][1], l, 0x400000);
        for (uint32_t k = 0; k < 0x200000; k++)
            ptrom[c * 0x200000 + k] = (uint32_t)(h[2 * k + 1] << 8 | h[2 * k]) << 16 | (uint32_t)(l[2 * k + 1] << 8 | l[2 * k]);
    }
    free(h); free(l);
    tmlrom = calloc(1, 0x400000); tmhrom = malloc(0x200000); texrom = malloc(0x2000000);
    uint8_t *raw = (uint8_t *)tmlrom;
    const int ccrl_half = s23_game.gorgon || s23_game.ccrl_half;
    if (ccrl_half) ok &= load(dir, s23_game.ccrl, raw, 0x200000);   /* GORgON, Panic Park: a 2 MB tile map (MAME textilemapl 0x200000) */
    else ok &= load(dir, s23_game.ccrl, raw, 0x400000);
    for (uint32_t k = 0; k < 0x200000; k++) tmlrom[k] = (uint16_t)(raw[2 * k] | raw[2 * k + 1] << 8);   /* ROM_REGION16_LE */
    if (ccrl_half) memcpy(tmlrom + 0x100000, tmlrom, 0x200000);   /* half-size map: mirror it, as MAME's tileid mask wraps */
    ok &= load(dir, s23_game.ccrh, tmhrom, 0x200000);
    const uint32_t cgsz = s23_game.cg_chip ? s23_game.cg_chip : 0x800000;
    for (int c = 0; c < 4; c++) ok &= load(dir, s23_game.cg[c], texrom + c * cgsz, cgsz);
    for (uint32_t o = 4 * cgsz; o < 0x2000000; o += 4 * cgsz) memcpy(texrom + o, texrom, 4 * cgsz);   /* a smaller set wraps */
    if (s23_game.spr[0] && (sprrom = malloc(0x1000000)))   /* GORgON's sprite tiles (MAME region "sprites") */
        for (int c = 0; c < 4; c++) ok &= load(dir, s23_game.spr[c], sprrom + c * 0x400000, 0x400000);
    tm_decoded =malloc(4u * (tileid_mask | 0xFF) + 4); tattr_decoded = malloc((tileid_mask | 0xFF) + 1);
    for (uint32_t id = 0; id <= (tileid_mask | 0xFF); id++) {
        uint8_t a = tmhrom[id >> 1]; a = (id & 1) ? (a & 15) : (a >> 4);
        tm_decoded[id] = (((uint32_t)tmlrom[id] | (uint32_t)(a & 1) << 16) & tile_mask) << 8;
        tattr_decoded[id] = a >> 1;
    }
    roms_ok = ok;
    if (ok) fprintf(stderr, "[3D] point ROM, texture tiles and tile maps loaded (model table: 0 -> %06X, 1 -> %06X)\n", ptrom[0], ptrom[1]);
}

/* the ROM data the GPU back end uploads (src/s23_gl.c) */
void s23_render_assets(const uint8_t **t, const uint32_t **m, const uint8_t **a)
{ *t = roms_ok ? texrom : NULL; *m = tm_decoded; *a = tattr_decoded; }

/* ---------------------------------------------------------------- the C404 (0x06A08000: 16-bit registers) and the palette, from the snapshot */
static const s23_frame_state *FS;
static int c404(int w) { return FS->c404[2 * w] << 8 | FS->c404[2 * w + 1]; }
static int c404b(int w) { return FS->c404[2 * w + 1]; }      /* most C404 registers keep 8 bits (MAME c404_t: u8 fields) -- the game writes
                                                         * e.g. its polygon fade as (rgb >> 16, rgb >> 8, rgb) unmasked: 00FF FFFF FFFF */
static inline uint32_t pen_rgb(uint32_t n) { return FS->pens[n & 0x7FFF]; }
uint32_t s23_photo_tex[S23_PHOTO_W * 256]; volatile int s23_photo_gen;   /* the photo page (include/s23_host.h) */

/* ---------------------------------------------------------------- polygons */
typedef s23_pvert pvert;
typedef s23_poly poly;
#define MAX_POLYS S23_MAX_POLYS
static s23_built *BLD;                                 /* the frame being built (the worker) */
#define GPOLYS (BLD->polys)
#define GNPOLY (BLD->npoly)
#define GFX (BLD->fx)

static int32_t s24(uint32_t v) { return (v & 0x800000) ? (int32_t)(v | 0xFF000000u) : (int32_t)(v & 0xFFFFFF); }
static int32_t s10(uint32_t v) { return (v & 0x200) ? (int32_t)((0x3FF - (v & 0x3FF)) | 0xFFFFFE00u) : (int32_t)(v & 0x1FF); }

static void transform(const s23_render_entry *e, int32_t xi, int32_t yi, int32_t zi, float *x, float *y, float *z)
{
    int16_t m[9]; memcpy(m, e->m, sizeof m);
    if (e->transpose) { int16_t t[9]; memcpy(t, m, sizeof t); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 3 + c] = t[c * 3 + r]; }
    *x = ((float)((m[0] * (int64_t)xi + m[1] * (int64_t)yi + m[2] * (int64_t)zi) >> 14) * e->scaling + e->v[0]) / 16384.f;
    *y = ((float)((m[3] * (int64_t)xi + m[4] * (int64_t)yi + m[5] * (int64_t)zi) >> 14) * e->scaling + e->v[1]) / 16384.f;
    *z = ((float)((m[6] * (int64_t)xi + m[7] * (int64_t)yi + m[8] * (int64_t)zi) >> 14) * e->scaling + e->v[2]) / 16384.f;
}
static void matrot(const s23_render_entry *e, int32_t xi, int32_t yi, int32_t zi, float *x, float *y, float *z)
{
    *x = (e->m[0] * xi + e->m[1] * yi + e->m[2] * zi) / 4194304.f;
    *y = (e->m[3] * xi + e->m[4] * yi + e->m[5] * zi) / 4194304.f;
    *z = (e->m[6] * xi + e->m[7] * yi + e->m[8] * zi) / 4194304.f;
}
/* the near plane: keep z >= zmin, interpolating x, y and the parameters (MAME poly_manager zclip_if_less) */
static int zclip(int n, const pvert *in, pvert *out, float zmin)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const pvert *a = &in[i], *b = &in[(i + 1) % n];
        const int ain = a->p[0] >= zmin, bin = b->p[0] >= zmin;
        /* bounded: a point-ROM polygon has up to 15 corners and the clip adds some -- the slot was 8 and overflowed into the next
         * polygon (Gunmen Wars' item drops: corrupt polygons that crashed Intel's GL driver) */
        if (ain && k < S23_POLY_MAXV) out[k++] = *a;
        if (ain != bin && k < S23_POLY_MAXV) {
            const float t = (zmin - a->p[0]) / (b->p[0] - a->p[0]);
            pvert *o = &out[k++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t;
            for (int q = 0; q < 4; q++) o->p[q] = a->p[q] + (b->p[q] - a->p[q]) * t;
        }
    }
    return k;
}
static void project(pvert *v, int vp_size_x, int vp_size_y, float fov)
{
    v->x = (float)vp_size_x + fov * v->x;
    v->y = (float)vp_size_y - fov * v->y;
    v->p[0] = 1.0f / v->p[0];
}
static void effects(poly *p, int color_for_alpha, int type_bit21, int h, int nofog)
{
    p->fog = !nofog;                                      /* the bit above the 7-bit palette: no depth fog (Super 22's C-fog gate) */
    p->alpha_enabled = (color_for_alpha & 0x7F) != GFX.alpha_color || type_bit21;
    p->blend = (h >> 10) & 1;
    p->prioverchar = 2;                                   /* text covered by a polygon is drawn again over it (MAME: 2 for polygons) */
}
/* the near plane: MAME's 0.0001; $S23_ZNEAR=<z> tries another (TEST ONLY, Crisis Zone's close-up smoke, crszone/PLAN.md) */
static float znear(void) { static float z = -1; if (z < 0) { S23_SS_ENV(z); const char *e = getenv("S23_ZNEAR"); z = e ? (float)atof(e) : s23_game.znear > 0.f ? s23_game.znear : 0.0001f; } return z; }

/* project the clipped vertices, find the z-sort key; 0 = dropped */
static long s23_why_from(void)   /* debug: $S23_WHYFROM=<frame> -- the [WHYP] trace starts there */
{
    static long f = -2; if (f == -2) { S23_SS_ENV(f); const char *e = getenv("S23_WHYFROM"); f = e ? atol(e) : 0; }
    return f;
}

/* (s23_game.h seg_order) the stretch a render entry belongs to, for ordering: mode 1 = its own; mode 2 = a stretch drawn through a WINDOW (a
 * viewport smaller than the screen) joins the stretch before it and sorts by depth with it */
static int seg_mode(void) { static int m = -1; if (m < 0) { S23_SS_ENV(m); const char *e = getenv("S23_SEGMODE"); m = e ? atoi(e) : s23_game.seg_order; } return m; }
static uint8_t seg_eff[256]; static int seg_seen = -1;
static int eff_seg(const s23_render_entry *e)
{
    const int s = e->seg;
    if (s == 0) { seg_seen = 0; seg_eff[0] = 0; }
    else if (s > seg_seen) {                              /* the first entry of a new stretch: the list runs in order */
        for (int k = seg_seen + 1; k <= s; k++) {
            const int windowed = seg_mode() == 2 && k == s && e->vp_size_x > 0 && e->vp_size_x < 300;
            seg_eff[k] = (uint8_t)(k == 0 ? 0 : windowed ? seg_eff[k - 1] : seg_eff[k - 1] + 1);
        }
        seg_seen = s;
    }
    return seg_eff[s] > 7 ? 7 : seg_eff[s];
}

static int finish(poly *p, const s23_render_entry *e, int hsort, uint32_t polyshift, int shift_bit, uint32_t shift_mask_lo, uint32_t shift_sign)
{
    { static int vh = -1; if (vh < 0) { S23_SS_ENV(vh); const char *ev = getenv("S23_VPHIDE"); vh = ev ? atoi(ev) : s23_game.vp_hide0; }   /* (s23_game.h vp_hide0) */
      if (vh && (e->vp_prio & 7) == 0) return 0; }
    float minz = FLT_MAX, maxz = FLT_MIN;
    for (int i = 0; i < p->n; i++) {
        float z = p->pv[i].p[0] != 0 ? p->pv[i].p[0] : 1.f;
        p->pv[i].x /= z; p->pv[i].y /= z;
        z *= 16384.f;
        if (z > maxz) maxz = z;
        if (z < minz) minz = z;
        { static float ft = -2; if (ft == -2) { S23_SS_ENV(ft); const char *ef = getenv("S23_FOVTEST"); ft = ef ? (float)atof(ef) : -1; }   /* TEST ONLY */
          project(&p->pv[i], e->vp_size_x, e->vp_size_y, ft > 0 && e->type == S23_RE_MODEL ? ft : e->vp_fov); }
        const float w = p->pv[i].p[0];
        p->pv[i].p[1] *= w; p->pv[i].p[2] *= w; p->pv[i].p[3] *= w;
    }
    if (maxz < 0) return 0;
    /* THE CLOSE-UP SMOKE (2026-10-12, Crisis Zone level 3; the user's report "the screen turns mostly grey"): a TRANSLUCENT polygon whose
     * projected corner lies beyond +-4096 pixels is not drawn. The explosion smoke there is ~58 screen-facing billboards 1-2 units from the
     * camera, each a 50% blend: drawn (as MAME draws them -- whose own comment says its near clipping is not the hardware's) they stack to
     * a solid grey for ~1.5 s. The arcade footage of the same room (youtube 8HKN2ZpNKic, 1112-1118 s) shows a light haze with the room
     * visible. Chosen against that footage over 10 paired scenes: a near plane at 2 units removed the cover panels; culling only past 16
     * bits left the grey; this keeps every opaque wall and floor (only blended polygons), the hit flash (a screen fade) and the haze.
     * The board's real limit is not documented: 4096 is the value that fits. S23_XYCULL=<n> tries another, 0 = off (MAME's picture). */
    { static float lim = -2; if (lim == -2) { S23_SS_ENV(lim); const char *e2 = getenv("S23_XYCULL"); lim = e2 ? (float)atof(e2) : s23_game.no_xycull ? 0.f : 4096.f; }
      static int all = -1; if (all < 0) all = getenv("S23_XYCULL_ALL") != NULL;   /* TEST ONLY: opaque polygons too */
      if (lim > 0 && (all || p->blend)) for (int i = 0; i < p->n; i++) if (p->pv[i].x > lim || p->pv[i].x < -lim || p->pv[i].y > lim || p->pv[i].y < -lim) return 0; }
    int zsort;
    switch (hsort) { case 0: zsort = (int)(minz + 0.5f); break; case 1: zsort = (int)(maxz + 0.5f); break; default: zsort = (int)(0.5f * (minz + maxz) + 0.5f); }
    if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
    int ap = e->absolute_priority & 7;
    if (polyshift >> 21 & 1) zsort = (int)(polyshift & 0x1FFFFF);
    else {
        static float bsc = -2; if (bsc == -2) { S23_SS_ENV(bsc); const char *eb2 = getenv("S23_ZBIAS_SCALE"); bsc = eb2 ? (float)atof(eb2) : 0.f; }   /* TEST: $S23_ZBIAS_SCALE scales every model's z bias (negative: subtracted) */
        const int zsc = s23_game.zbias_scale && e->type == S23_RE_MODEL ? s23_game.zbias_scale(e->model) : 0;   /* the game's own, per model */
        zsort += (int)((shift_bit != 17 ? 1.f : bsc != 0.f ? bsc : zsc != 0 ? (float)zsc : 1.f) * (float)((polyshift >> shift_bit & 1) ? (int)(polyshift | shift_sign) : (int)(polyshift & shift_mask_lo)));
        ap += (int)((polyshift & 0x1C0000) >> 18);
		if (s23_game.ap_wrap && e->type == S23_RE_MODEL) ap &= 7;   /* models only (include/s23_game.h ap_wrap) */
    }
    { static int zm = -1; if (zm < 0) { S23_SS_ENV(zm); const char *ez = getenv("S23_ZOFFMODE"); zm = ez ? atoi(ez) : s23_game.zoff_state; }   /* (s23_game.h
       * zoff_state) the C435's state 9 depth: 6 = signed, added (Panic Park); tests: 1 subtracts, 2 adds unsigned, 3 scales, 4 maps 0..0x1FFFFF onto it..0x1FFFFF, 5 adds to the stretch before */
      if (zm == 1) zsort -= e->zoff; else if (zm == 2) zsort += e->zoff;
      else if (zm == 3 && e->zoff) zsort = (int)((int64_t)zsort * e->zoff / 0x1FFFFF);
      else if (zm == 4) zsort = e->zoff + (int)((int64_t)zsort * (0x1FFFFF - e->zoff) / 0x1FFFFF);
      else if (zm == 5 && e->seg < 64) zsort += FS->list.seg_zoff[e->seg];
      else if (zm == 6) zsort += 0x8000 + ((e->zoff & 0x3FFFF) ^ 0x20000) - 0x20000; }   /* 6: an 18-bit SIGNED offset (+20000, -500 ...), added;
                                                   * everything moved back 0x8000 so a negative one keeps its layers' order instead of all clamping at 0 */
    { static float zr = -1.f; if (zr < 0.f) { S23_SS_ENV(zr); const char *er = getenv("S23_WINZREF"); zr = er ? (float)atof(er) : s23_game.win_zref; }   /* (s23_game.h win_zref) */
      if (zr > 0.f && e->seg >= s23_game.win_zref_from && e->vp_fov > 0.f && e->vp_fov < zr) zsort = (int)((float)zsort * e->vp_fov / zr); }
    if (zsort < 0) zsort = 0;
    if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
    p->zkey = (uint32_t)zsort | (uint32_t)ap << 21;
    if (seg_mode()) p->zkey |= (uint32_t)(7 - eff_seg(e)) << 25;   /* (s23_game.h seg_order) a later stretch of the list draws over an earlier one
                                                   * (bits 25-27: the priority can reach 13 = bits 21-24; the band zkey >> 21 stays under 128) */
    { static int vm = -1; if (vm < 0) { S23_SS_ENV(vm); const char *ev = getenv("S23_VPPRIO"); vm = ev ? atoi(ev) : s23_game.vp_front0 ? 4 : 0; }
      /* (s23_game.h vp_front0) $S23_VPPRIO: 4 = the viewport word's low 3 bits as the first key, lower in front (Panic Park), 5 = only 0 in front */
      static int wl = -1; if (wl < 0) { S23_SS_ENV(wl); const char *ew = getenv("S23_WINLAYER"); wl = ew ? atoi(ew) : s23_game.win_layer; }
      int layer = 0;                                    /* higher = in front (bits 25-27 hold 7 - layer) */
      if (wl) {                                         /* (s23_game.h win_layer) */
          static const void *wl_fs; static int wl_idx, wl_seg, wl_nwin, wl_segwin;
          const int idx = (int)(e - FS->list.e);
          if ((const void *)FS != wl_fs || idx < wl_idx) { wl_fs = FS; wl_seg = -1; wl_nwin = 0; wl_segwin = 0; }
          wl_idx = idx;
          const int win = e->vp_size_x != 320 || e->vp_size_y != 240 || e->vp_offset_x || e->vp_offset_y;
          if (e->seg != wl_seg) { if (wl_seg >= 0 && wl_segwin) wl_nwin++; wl_seg = e->seg; wl_segwin = 0; }
          if (win) wl_segwin = 1;
          layer = wl_nwin + win; if (layer > 5) layer = 5;
      }
      if (vm == 4) p->zkey |= (uint32_t)(e->vp_prio & 7) << 25;
      else if (vm == 5 && (e->vp_prio & 7) == 0) layer = 6;
      if (vm != 4 && (wl || vm == 5)) p->zkey |= (uint32_t)(7 - layer) << 25; }
    p->index = (uint32_t)GNPOLY;
    p->vp_size_x = e->vp_size_x; p->vp_size_y = e->vp_size_y; p->vp_offset_x = e->vp_offset_x; p->vp_offset_y = e->vp_offset_y;
    p->clip43 = e->clip43;
    p->entry = (int)(e - FS->list.e); p->nodepth = 0; p->flat = e->type != S23_RE_MODEL; p->model = e->type == S23_RE_MODEL ? e->model : -1; p->role = p->model >= 0 && s23_game.depth_role ? s23_game.depth_role(p->model) : 0;
    return 1;
}

/* FIGHTER VIEW (Display menu, games with s23_game.znear_model: Gunmen Wars), saved as first_person in the game's cfg: 0 = your own
 * fighter drawn (the default, the user's choice), 1 = first person -- the near plane hides your fighter, as the footage shows */
int s23_first_person;
static void render_model(const s23_render_entry *e)
{
    const int blend = e->model2 != 0;
    { static long lo = -2, hi; if (lo == -2) { S23_SS_ENV(lo); S23_SS_ENV(hi); const char *ek = getenv("S23_MODELKEEP"); lo = -1; if (ek) sscanf(ek, "%lx:%lx", &lo, &hi); }   /* TEST ONLY: draw only models lo..hi */
      if (lo >= 0 && (e->model < lo || e->model > hi)) return; }
    const uint32_t adr = ptrom[e->model], adr2 = ptrom[e->model2];
    if (adr >= ptrom_limit || adr2 >= ptrom_limit) {   /* a model the point ROM has no data for ($S23_MODELLOG: which) */
        static int lg = -1, n; if (lg < 0) { S23_SS_ENV(lg); lg = getenv("S23_MODELLOG") != NULL; }
        if (lg && n++ < 40) fprintf(stderr, "[MODEL] dropped model %04X (%06X) model2 %04X (%06X)\n", e->model, adr, e->model2, adr2);
        return; }
    { static int lg = -1, n; if (lg < 0) { S23_SS_ENV(lg); lg = getenv("S23_MODELLOG") != NULL; }     /* a model whose data lies past the chips fitted */
      const uint32_t fitted = (uint32_t)s23_game.npt * 0x200000u;
      if (lg && (adr >= fitted || adr2 >= fitted) && n++ < 40) fprintf(stderr, "[MODEL] model %04X data at %06X (model2 %04X %06X): past the %d chip pairs\n", e->model, adr, e->model2, adr2, s23_game.npt); }
    /* $S23_MODELIDS=from:to (debug): per model id, the entries submitted and the polygons they produced in those frames, at frame <to> */
    static long mi_from = -2, mi_to; static uint32_t mi_ent[65536], mi_poly[65536]; static int mi_done;
    if (mi_from == -2) { S23_SS_ENV(mi_from); S23_SS_ENV(mi_to); S23_SS_ENV(mi_done); mi_from = -1; const char *e3 = getenv("S23_MODELIDS"); if (e3) sscanf(e3, "%ld:%ld", &mi_from, &mi_to); }
    extern uint32_t rr_frame;
    const int mi_on = mi_from >= 0 && (long)rr_frame >= mi_from && (long)rr_frame <= mi_to;
    if (mi_from >= 0 && !mi_done && (long)rr_frame > mi_to) { mi_done = 1;
        for (int k = 0; k < 65536; k++) if (mi_ent[k]) fprintf(stderr, "[MODELID] %04X entries %u polys %u\n", k, mi_ent[k], mi_poly[k]); }
    const int mi_p0 = GNPOLY;
    if (mi_on) mi_ent[e->model]++;
    const uint32_t *data = &ptrom[adr], *data2 = &ptrom[adr2];
    uint32_t offs = 0;
    /* the blend factor is SIGNED (1.0 = 0x4000): Downhill Bikers' dust puffs count it down from 0x2FFF through 0 to small negatives as
     * they age (0xF810 = -0.12) -- read unsigned (MAME's), that was 3.9: the puff stretched into screen-high cream slabs over the course */
    const float fb_ = (int16_t)e->model_blend_factor / 16384.f, fa_ = 1.f - fb_;
    while (adr + offs < ptrom_limit && adr2 + offs < ptrom_limit && GNPOLY < MAX_POLYS) {
        pvert pv[16];
        const uint32_t type = data[offs++], h = data[offs++];
        const int cmode = (int)((type & 0x70000000u) >> 28);
        const int tbase = (int)(((type & 0x0F000000u) >> 24) << 12 | (type >> 31) << 16);
        const int color = (int)((h >> 24) & 0x7F), lmode = (int)((type >> 19) & 3), ne = (int)((type >> 8) & 15), stencil = (int)((h >> 11) & 1);
        uint32_t polyshift = 0;
        if (type & 0x1000) polyshift = data[offs++];
        uint32_t light = 0, extptr = 0;
        if (lmode == 3) { extptr = offs; offs += (uint32_t)ne; } else light = data[offs++];
        { static long why = -2, why2; static int nw; if (why == -2) { S23_SS_ENV(why); S23_SS_ENV(why2); S23_SS_ENV(nw); const char *e4 = getenv("S23_WHYMODEL"); char *en = NULL; why = e4 ? strtol(e4, &en, 16) : -1; why2 = en && *en == '-' ? strtol(en + 1, NULL, 16) : why; }   /* debug: id or lo-hi */
          if (e->model >= why && e->model <= why2 && why >= 0 && (long)rr_frame >= s23_why_from() && nw++ < 4000) fprintf(stderr, "[WHYP] f%u ps %08X m %04X type %08X h %08X cmode %d tbase %05X color %02X lmode %d light %08X uv %X,%X %X,%X %X,%X %X,%X\n", rr_frame, polyshift, e->model, type, h, cmode, tbase, color, lmode, light,
              ((data[offs] >> 20) & 0xF00) | ((data[offs + 1] >> 24) & 0xFF), ((data[offs] >> 16) & 0xF00) | ((data[offs + 2] >> 24) & 0xFF),
              ((data[offs + 3] >> 20) & 0xF00) | ((data[offs + 4] >> 24) & 0xFF), ((data[offs + 3] >> 16) & 0xF00) | ((data[offs + 5] >> 24) & 0xFF),
              ((data[offs + 6] >> 20) & 0xF00) | ((data[offs + 7] >> 24) & 0xFF), ((data[offs + 6] >> 16) & 0xF00) | ((data[offs + 8] >> 24) & 0xFF),
              ((data[offs + 9] >> 20) & 0xF00) | ((data[offs + 10] >> 24) & 0xFF), ((data[offs + 9] >> 16) & 0xF00) | ((data[offs + 11] >> 24) & 0xFF)); }
        for (int i = 0; i < ne && i < 16; i++) {
            const uint32_t v1 = data[offs], v2 = data[offs + 1], v3 = data[offs + 2];
            float x, y, z;
            transform(e, s24(v1), s24(v2), s24(v3), &x, &y, &z);
            if (blend) {
                float x2, y2, z2;
                transform(e, s24(data2[offs]), s24(data2[offs + 1]), s24(data2[offs + 2]), &x2, &y2, &z2);
                x = x * fa_ + x2 * fb_; y = y * fa_ + y2 * fb_; z = z * fa_ + z2 * fb_;
            }
            offs += 3;
            { static long fz = -2; if (fz == -2) { const char *ef = getenv("S23_FLIPZ"); fz = ef ? strtol(ef, NULL, 16) : -1; }   /* TEST: this model's z negated */
              if (fz == e->model) z = -z; }
            pv[i].x = x; pv[i].y = y; pv[i].p[0] = z;
            const float half = (stencil || s23_game.uv_edge) ? 0 : 0.5f;   /* MAME's half texel; uv_edge: none (the far edge stays inside the tile) */
            pv[i].p[1] = (float)(((v1 >> 20) & 0xF00) | ((v2 >> 24) & 0xFF)) + half + e->tx;
            pv[i].p[2] = (float)(((v1 >> 16) & 0xF00) | ((v3 >> 24) & 0xFF)) + half + e->ty;
            { static int um = -2; static int nlog; if (um == -2) { const char *e2 = getenv("S23_UVLOG"); um = e2 ? atoi(e2) : -1; }   /* TEST: a model's raw u/v */
              if (e->model == um && nlog < 64) { nlog++; fprintf(stderr, "[UV] v%d u %u v %u tx %d ty %d stencil %d\n", i, (unsigned)(((v1 >> 20) & 0xF00) | ((v2 >> 24) & 0xFF)), (unsigned)(((v1 >> 16) & 0xF00) | ((v3 >> 24) & 0xFF)), e->tx, e->ty, stencil); } }
            pv[i].p[3] = 64;
            static const uint8_t shifts[4] = { 24, 16, 8, 0 };
            if (lmode <= 1) pv[i].p[3] = (float)((light >> shifts[i & 3]) & 0xFF);
            else if (lmode == 3) {
                const uint32_t norm = data[extptr];
                int32_t nx = s10((norm >> 20) & 0x3FF), ny = s10((norm >> 10) & 0x3FF), nz = s10(norm & 0x3FF);
                if (blend) {
                    const uint32_t n2 = data2[extptr];
                    nx = (int32_t)(nx * fa_ + s10((n2 >> 20) & 0x3FF) * fb_); ny = (int32_t)(ny * fa_ + s10((n2 >> 10) & 0x3FF) * fb_); nz = (int32_t)(nz * fa_ + s10(n2 & 0x3FF) * fb_);
                }
                extptr++;
                float nrx, nry, nrz, cx, cy, cz;
                matrot(e, nx, ny, nz, &nrx, &nry, &nrz);
                float len = sqrtf(nrx * nrx + nry * nry + nrz * nrz); if (len != 0) { nrx /= len; nry /= len; nrz /= len; }
                matrot(e, e->light_vector[0], e->light_vector[1], e->light_vector[2], &cx, &cy, &cz);
                len = sqrtf(cx * cx + cy * cy + cz * cz); if (len != 0) { cx /= len; cy /= len; cz /= len; }
                float lsi = nrx * cx + nry * cy + nrz * cz; if (lsi < 0) lsi = 0;
                float s = e->light_ambient + e->light_power * lsi; if (s < 0) s = 0; if (s > 64) s = 64;
                pv[i].p[3] = s;
            }
        }
        if (ne < 3) { if (type & 0x10000) break; continue; }
        static int nobf = -1; if (nobf < 0) { S23_SS_ENV(nobf); nobf = getenv("S23_NOBACKFACE") != NULL; }   /* TEST ONLY: draw back faces too */
        if ((h & 0x20) && !nobf) {                         /* back-face test on the transformed quad */
            const float z0 = pv[0].p[0], z1 = pv[1].p[0], z2 = pv[2].p[0], z3 = pv[3].p[0];
            const float c1 = pv[2].x * (z0 * pv[1].y - pv[0].y * z1) + pv[2].y * (pv[0].x * z1 - z0 * pv[1].x) + z2 * (pv[0].y * pv[1].x - pv[0].x * pv[1].y);
            const float c2 = pv[0].x * (z2 * pv[3].y - pv[2].y * z3) + pv[0].y * (pv[2].x * z3 - z2 * pv[3].x) + z0 * (pv[2].y * pv[3].x - pv[2].x * pv[3].y);
            if (c1 >= 0.f && c2 >= 0.f) { if (type & 0x10000) break; continue; }
        }
        if (e->mirror_x) for (int i = 0; i < 4; i++) pv[i].x = -pv[i].x;
        { static float rej = -2; if (rej == -2) { const char *e2 = getenv("S23_ZREJECT"); rej = e2 ? (float)atof(e2) : -1; }   /* TEST ONLY: drop a BLENDED polygon with a vertex nearer than <z> */
          if (rej >= 0 && (h >> 10 & 1)) { int nearv = 0; for (int i = 0; i < ne; i++) nearv |= pv[i].p[0] < rej; if (nearv) { if (type & 0x10000) break; continue; } } }
        poly *p = &GPOLYS[GNPOLY];
        p->n = zclip(ne, pv, p->pv, s23_first_person && e->vp_fov < 500.f && s23_game.znear_model > znear() ? s23_game.znear_model : znear());   /* first-person view (Display menu): the game's near plane for 3D-view models (not the 1:1 overlay ones) */
        { static long why = -2; static int nw; if (why == -2) { S23_SS_ENV(why); S23_SS_ENV(nw); const char *e4 = getenv("S23_WHYMODEL"); why = e4 ? strtol(e4, NULL, 16) : -1; }   /* debug */
          if (why == e->model && (long)rr_frame >= s23_why_from() && nw++ < 12) fprintf(stderr, "[WHY] f%u m %04X ne %d z %g %g %g x %g y %g clip-n %d scale %g v %g %g %g vp %d %d fov %g\n", rr_frame, e->model, ne, pv[0].p[0], pv[1].p[0], pv[2].p[0], pv[0].x, pv[0].y, p->n,
              e->scaling, e->v[0], e->v[1], e->v[2], e->vp_size_x, e->vp_size_y, e->vp_fov); }
        p->blend = (int)((h >> 10) & 1);                    /* known before finish() (the S23_XYCULL_BLEND test); effects() sets it again */
        if (p->n >= 3 && finish(p, e, (int)((h & 0x300) >> 8), polyshift, 17, 0x1FFFF, 0xFFFC0000u)) {
            p->stencil = stencil; p->cmode = cmode; p->tbase = tbase; p->color_base = color << 8; p->boost = 0;
            effects(p, color, (int)((type >> 21) & 1), (int)h, (int)((h >> 31) & 1));
            if (s23_game.over_text && s23_game.over_text(e->model)) p->prioverchar = 7;   /* the game's own: over the text layer */
            GNPOLY++;
        }
        if (type & 0x10000) break;
    }
    if (mi_on) mi_poly[e->model] += (uint32_t)(GNPOLY - mi_p0);
}

static void render_immediate(const s23_render_entry *e)
{
    const s23_immediate *q = &e->imm;
    const uint32_t type = q->type, h = q->h;
    const int ne = (int)((type >> 8) & 0xF);
    pvert pv[16];
    for (int i = 0; i < ne && i < 4; i++) {
        /* 24-BIT signed positions (like the point ROM's): Gunmen Wars' title logo sends -0.008 as 0x00FFFF80 -- read as a 32-bit value
         * (as MAME does) it became +1024 and the logo was projected far off-screen */
        pv[i].x = s24(q->x[i]) / 16384.f; pv[i].y = s24(q->y[i]) / 16384.f; pv[i].p[0] = s24(q->z[i]) / 16384.f;
        /* u / v: the LOW 16 BITS only (Gunmen Wars' namco logo: 300 triangles whose words carry junk in the high half -- u 79C60A00,
         * v 022A7100 -- read whole they sampled noise all over the texture ROM; masked they read the ball's metallic map, as the arcade.
         * The ball / letters (3-corner immediates, header 4F2C) reflect a dusk-sky panorama (page 7, around u 0A00 v 7100: S23_TEXDUMP);
         * the flat letters all read u 0A00 v 7100, the dark sky's edge. The ARCADE turns them a bright even red-orange the moment they
         * flatten (video 1, 159.6 -> 160.0 s): not found -- ruled out: fewer v bits, 2 / 4 fraction bits, v offsets 0x60 / 0xC0 (brown with
         * skyline), palette / C404 / text layer / C412-C417 writes (all constant over the logo). Likely a C435 / C404 feature not emulated) */
        pv[i].p[1] = (float)(q->u[i] & 0xFFFF); pv[i].p[2] = (float)(q->v[i] & 0xFFFF); pv[i].p[3] = (float)(int32_t)q->i[i];    }
    int boost = 0;
    int uvp[3] = { 0, 0, -1 };
    if (s23_game.imm_adjust) { float va = 0.f; s23_game.imm_adjust(q, &va, &boost, uvp);
        for (int i = 0; i < ne && i < 4; i++) { pv[i].p[2] += va; if (boost) pv[i].p[3] *= 2.f; if (uvp[2] >= 0) { pv[i].p[1] = (float)uvp[0]; pv[i].p[2] = (float)uvp[1]; } } }
    /* ONE TEXEL for the whole polygon (every corner the same u / v: a flat fill, Crisis Zone's tank shadow at u 0x1000 v 0x0607): sample
     * that texel's centre. On its corner, the rounding of the perspective-correct interpolation picked the neighbours -- cyan, orange,
     * magenta pens -- instead of the black one (the user's capture, level 1 boss). A textured polygon is unchanged. */
    if (uvp[2] < 0 && ne >= 3) { int same = 1;
        for (int i = 1; i < ne && i < 4; i++) same &= pv[i].p[1] == pv[0].p[1] && pv[i].p[2] == pv[0].p[2];
        if (same) for (int i = 0; i < ne && i < 4; i++) { pv[i].p[1] += 0.5f; pv[i].p[2] += 0.5f; } }
    if (GNPOLY >= MAX_POLYS) return;
    poly *p = &GPOLYS[GNPOLY];
    p->n = zclip(ne < 4 ? ne : 4, pv, p->pv, znear());
    { static int lg = -1, n, lg2; if (lg < 0) { const char *el = getenv("S23_IMMLOG"); lg = el ? (*el == '3' ? 3 : 1) : 0; lg2 = el && *el == '2'; }   /* debug: how an immediate polygon decodes (=2: the first 4 of every frame) */
      extern uint32_t rr_frame;
      static long from = -2; if (from == -2) { const char *ef = getenv("S23_IMMFROM"); from = ef ? atol(ef) : 3090; }
      static uint32_t lf; static int pf; if (rr_frame != lf) { lf = rr_frame; pf = 0; }
      if (lg && (long)rr_frame >= from && (lg2 ? pf++ < 4 : n++ < (lg == 3 ? 3000 : 300))) fprintf(stderr, "[IMMR] f%u type %08X h %08X ne %d v0 %g %g z %g v1 %g %g z %g v2 %g %g z %g clip-n %d pal %X | u/v %08X %08X %08X %08X %08X %08X | i %08X %08X\n", rr_frame, type, h, ne,
          pv[0].x, pv[0].y, pv[0].p[0], pv[1].x, pv[1].y, pv[1].p[0], pv[2].x, pv[2].y, pv[2].p[0], p->n, (unsigned)q->pal,
          (unsigned)q->u[0], (unsigned)q->v[0], (unsigned)q->u[1], (unsigned)q->v[1], (unsigned)q->u[2], (unsigned)q->v[2], (unsigned)q->i[0], (unsigned)q->i[1]); }
    p->blend = (int)((h >> 10) & 1);
    if (p->n >= 3 && finish(p, e, 2, q->zbias, 16, 0xFFFF, 0xFFFE0000u)) {
        p->stencil = (int)((h >> 11) & 1); p->cmode = 0; p->tbase = 0; p->boost = boost;
        for (int i = 1; i < ne && i < 4; i++) if (q->z[i] != q->z[0]) p->flat = 0;   /* corners at different depths: 3D (s23_poly.flat) */
        /* THE PHOTO SQUARE of an immediate (Gunmen Wars' diamond graph: u 0x8000-0x8080, v 0xF000-0xF080 -- u bit 15 set, the area starting
         * at u 0, v 0xF000): drawn from the photo page's 128 x 128 player picture, as the team select's model slot. Only that square: the
         * graph's line (u 0x8020, v 0xF080, also bit 15) reads its red from the texture ROM as before */
        if (s23_game.photo_page) { uint32_t mu = 0xFFFFFFFFu, mv = 0xFFFFFFFFu; int all15 = 1;
            for (int i = 0; i < q->n && i < 4; i++) { all15 &= (q->u[i] >> 15) & 1; if ((q->u[i] & 0x7FFF) < mu) mu = q->u[i] & 0x7FFF; if ((q->v[i] & 0xFFFF) < mv) mv = q->v[i] & 0xFFFF; }
            if (all15 && mu < 0x80 && mv >= 0xF000 && mv < 0xF080) {
                for (int i = 0; i < p->n; i++) p->pv[i].p[1] -= 32768.f * p->pv[i].p[0];   /* (u is stored times 1/z) */
                p->tbase = s23_game.photo_page - 0xF000; } } p->color_base = uvp[2] >= 0 ? uvp[2] << 8 : (int)(q->pal & 0x7F00);
        effects(p, (int)((q->pal >> 8) & 0x7F), 0, (int)h, (int)((q->pal >> 15) & 1));
        GNPOLY++;
    }
}

/* a DIRECT quad (MAME render_direct_poly): two triangles (0 1 2, 0 2 3), already projected */
static void render_direct(const s23_render_entry *e)
{
    const uint16_t *d = e->d;
    const uint32_t polyshift = (uint32_t)(d[1] & 0x1FF) << 12 | (d[0] & 0xFFF);
    static const int idx[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int t = 0; t < 2 && GNPOLY < MAX_POLYS; t++) {
        poly *p = &GPOLYS[GNPOLY];
        p->n = 3;
        for (int j = 0; j < 3; j++) {
            const uint16_t *src = &d[4 + idx[t][j] * 6];
            const int mant = src[5]; int ex = src[4] & 0x3F;
            float w = 1.f;
            if (mant) { w = (float)mant; while (ex < 0x2E) { w /= 2.0f; ex++; } }
            p->pv[j].p[0] = w;
            p->pv[j].p[1] = ((src[0] >> 4) + 0.5f) * w;
            p->pv[j].p[2] = ((src[1] >> 4) + 0.5f + (float)s23_game.direct_v_shift) * w;   /* (s23_game.h direct_v_shift) */
            p->pv[j].p[3] = (float)(src[4] >> 8) * w;
            p->pv[j].x = (float)((int16_t)src[2] + 320);
            p->pv[j].y = (float)((int16_t)src[3] + 240);
        }
        int zsort = 0, ap = e->absolute_priority & 7;
        if (polyshift >> 21 & 1) zsort = (int)(polyshift & 0x1FFFFF);
        else { zsort += (polyshift >> 17 & 1) ? (int)(polyshift | 0xFFFC0000u) : (int)(polyshift & 0x1FFFF); ap += (int)((polyshift & 0x1C0000) >> 18); }
        if (zsort < 0) zsort = 0;
        if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
        p->zkey = (uint32_t)zsort | (uint32_t)ap << 21; p->index = (uint32_t)GNPOLY;
        if (seg_mode()) p->zkey |= 7u << 25;
        { static int vm = -1; if (vm < 0) { S23_SS_ENV(vm); const char *ev = getenv("S23_VPPRIO"); vm = ev ? atoi(ev) : s23_game.vp_front0 ? 4 : 0; } if (vm == 4 || vm == 5 || s23_game.win_layer) p->zkey |= 7u << 25; }   /* (the direct buffer: behind every model, as before)   /* a direct polygon (the CPU's own buffer, not the command list): with the first stretch -- Panic Park's
                                                   * full-screen backdrop quad (priority 13) came in mid-list and covered the namco logo and the title */
        p->stencil = 0; p->color_base = d[2] & 0x7F00;
        p->tbase = (d[2] & 0xF) << 12 | ((d[2] >> 7) & 1) << 16;
        p->cmode = (d[2] & 0x70) >> 4;
        p->vp_size_x = e->vp_size_x; p->vp_size_y = e->vp_size_y; p->vp_offset_x = e->vp_offset_x; p->vp_offset_y = e->vp_offset_y;
        p->clip43 = 0; p->boost = 0;
        p->entry = (int)(e - FS->list.e); p->model = -1; p->role = 0; p->flat = 1;
        effects(p, (d[2] >> 8) & 0x7F, 0, 0, (d[2] >> 15) & 1);
        p->fog = 0;                                       /* MAME: a direct polygon's fog factor is 0 (screen-space panels: Crisis Zone's RULES) */
        p->blend = 0;
        p->prioverchar = 2;                               /* MAME 0.289: render_direct_poly works out (cmode == 1 ? 7 : 0), but render_flush draws EVERY
                                                          * polygon with prioverchar 2 -- text the polygon covers is drawn again over it (Angler King's NOTICE) */
		p->nodepth = p->pv[0].p[0] == 1.f && p->pv[1].p[0] == 1.f && p->pv[2].p[0] == 1.f;   /* no depth given (mantissa 0): out of exact depth */
        GNPOLY++;
    }
}

/* ---------------------------------------------------------------- the software fill (the fallback, and the GPU's oracle) */
static uint32_t *fb;
static uint8_t *prio;
static struct { int fade, fadefactor, fadefactor_inv, fr, fg, fb, pfade, pr, pg, pb, alpha, alpha_inv, alpha_pen, fog, cr, cg, cb; const uint8_t *fogtab; } rx;   /* from the frame's C404 */
static uint32_t tex_lookup(uint32_t pbase, int penshift, int penmask, uint32_t u, uint32_t v, uint8_t *pen)
{
    const uint32_t id = ((u >> 4) & 0xFF) | ((v << 4) & tileid_mask);
    const uint32_t tile = tm_decoded[id], a = tattr_decoded[id];
    if (a & 1) v = ~v;
    if (a & 2) u = ~u;
    if (a & 4) { const uint32_t t = u; u = v; v = t; }
    *pen = texrom[(tile | ((v << 4) & 0xF0) | (u & 0x0F)) & 0x1FFFFFF];
    return pen_rgb(pbase + ((uint32_t)(*pen >> penshift) & (uint32_t)penmask));
}
static int stencil_lookup(uint32_t x, uint32_t y)
{
    const uint32_t bit = (x & 15) ^ 15, offs = ((y << 6) | (x >> 4)) & 0x1FFFF;
    return !(FS->sram[offs] >> bit & 1);
}
/* EXACT DEPTH (2026-10-08, the user's request -- MAME sorts like the default and shows the same jumps): the hardware draws whole polygons
 * far to near by ONE depth key each, so neighbouring objects whose keys nearly tie swap places as the camera moves (measured: distant
 * buildings and the bridge in stage 1's boat chase jump in front of each other). With depth_mode on, a pixel is also depth-tested
 * against what a DIFFERENT object of the SAME priority band drew there (1/z, the nearer wins; a tie stays with the draw order); within
 * one object the game's order stands (its painted-on details), and other priority bands (the HUD ...) stay purely sorted. Translucent
 * and blended pixels are tested but write no depth. */
int s23_depth_mode = -1;
static float zb[640 * 480]; static int16_t eb[640 * 480]; static uint8_t ab[640 * 480];   /* the NEAREST opaque pixel: 1/z, entry + 1, band */
static int near_pass;                                   /* 1: span() only records the nearest opaque pixel (the pre-pass) */
static float depth_keep = 0.98f;                       /* a pixel is hidden only if the other object is nearer by more than this margin ($S23_DEPTH_TOL, percent) */
int s23_depth_on(void)                                   /* the setting (Display > Depth; $S23_DEPTH=0/1 for tests), shared with src/s23_gl.c */
{
    if (s23_depth_mode < 0) { const char *e = getenv("S23_DEPTH"); s23_depth_mode = e ? *e == '1' : s23_game.depth_default;
                              e = getenv("S23_DEPTH_TOL"); if (e) depth_keep = 1.0f - (float)atof(e) / 100.0f;
                              if (getenv("S23_DEPTH")) fprintf(stderr, "[S23] depth mode %d, keep %.4f\n", s23_depth_mode, depth_keep); }
    return s23_game.no_depth ? 0 : s23_depth_mode;
}
float s23_depth_keep(void) { s23_depth_on(); return depth_keep; }
void s23_depth_set(int on) { s23_depth_on(); if (!getenv("S23_DEPTH")) s23_depth_mode = on != 0; }   /* the menu / tc2.cfg; $S23_DEPTH wins for tests */
static int seamtest = -1;                                   /* $S23_SEAMTEST=1: coverage only -- each pixel = the drawn polygon's depth key (magenta = nothing), no text */
static void span(const poly *p, int y, int x0, int x1, const float *s, const float *d)
{
    const int clip_left = (320 - p->vp_size_x) + p->vp_offset_x, clip_right = (320 + p->vp_size_x) + p->vp_offset_x;
    const int clip_top = (240 - p->vp_size_y) - p->vp_offset_y, clip_bottom = (240 + p->vp_size_y) - p->vp_offset_y;
    const int oy = y + clip_top;
    if (oy < 0 || oy >= 480 || oy < clip_top || oy >= clip_bottom) return;
    uint32_t *dest = fb + oy * 640; uint8_t *pm = prio + oy * 640;
    uint32_t pbase = (uint32_t)p->color_base; int penmask = 0xFF, penshift = 0;
    if (p->cmode & 4) { pbase += 0xEC + ((p->cmode & 8) << 1); penmask = 3; penshift = 2 * (~p->cmode & 3); }
    else if (p->cmode & 2) { pbase += 0xE0 + ((p->cmode & 8) << 1); penmask = 0xF; penshift = 4 * (~p->cmode & 1); }
    float z = s[0], u = s[1], v = s[2], i = s[3];
    for (int x = x0; x < x1; x++, z += d[0], u += d[1], v += d[2], i += d[3]) {
        const float ooz = 1.0f / z;
        uint32_t tx = (uint32_t)(int32_t)(u * ooz), ty = (uint32_t)(int32_t)(v * ooz);
        const int vx = x + clip_left;
        if (vx < clip_left || vx >= clip_right || vx < 0 || vx >= 640) continue;
        { static int hide = -2; if (hide == -2) { S23_SS_ENV(hide); const char *e = getenv("S23_HIDEMODEL"); hide = e ? atoi(e) : -3; } if (p->model == hide) continue; }   /* TEST: leave one model out (-3: none -- -1 is every immediate and direct polygon) */
        { static int only = -2; if (only == -2) { S23_SS_ENV(only); const char *e = getenv("S23_ONLYMODEL"); only = e ? atoi(e) : -1; } if (only >= 0 && p->model != only) continue; }   /* TEST: one model only */
        if (seamtest == 3) {                              /* 3: OVERDRAW of translucent polygons (blend / alpha) -- 0x20 a layer; opaque ones not drawn */
            static int only = -2; if (only == -2) { const char *e = getenv("S23_SEAMMODEL"); only = e ? atoi(e) : -1; }   /* only this model */
            if ((only < 0 ? (p->blend || p->alpha_enabled) : p->model == only)) { if (!(p->stencil && stencil_lookup(tx, ty))) { const uint32_t c = dest[vx] == 0xFF00FF ? 0 : dest[vx] & 0xFF; dest[vx] = (c + 0x20 > 0xFF ? 0xFF : c + 0x20) * 0x010101u; } }
            continue; }
        if (seamtest) {                                   /* coverage: a stencil cutout is the art's own hole, not a seam -- drawn solid */ dest[vx] = seamtest == 2 ? (uint32_t)(p->entry & 0xFFF) << 12 | ((p->zkey & 0x1FFFFF) >> 9) : (p->zkey & 0xFFFFFF); continue; }   /* 1: depth key; 2: entry << 12 | depth >> 9 */
        if (p->stencil && stencil_lookup(tx, ty)) continue;
        if ((s23_depth_mode || s23_game.depth_within) && !p->nodepth) {   /* depth_within: within one object only, unless the menu's Depth is on too */
            const int k = oy * 640 + vx, ent = (int)(p->entry & 0x3FFF) + 1, band = (int)(p->zkey >> 21);
            if (near_pass) { if (p->role != 2 && z > zb[k]) { zb[k] = z; eb[k] = (int16_t)ent; ab[k] = (uint8_t)(band | (p->role == 1) << 7); } continue; }   /* (opaque polygons only; bit 7: role 1) */
            if (p->role == 2) { if (eb[k] && (ab[k] & 0x80)) continue; }   /* s23_game.depth_role: under the owner, wherever it is nearest */
            else if (p->role == 1 && eb[k] && eb[k] != ent && !(ab[k] & 0x80)) { }   /* an owner is never hidden by another model (the raft's floor sits under the water surface) */
            else if (eb[k] && (ab[k] & 0x7F) == band && (eb[k] != ent ? (s23_depth_mode || s23_game.depth_within >= 2) : s23_game.depth_within) && z < zb[k] * depth_keep) continue;   /* (depth_within: an object's own polygons; the menu: other objects) */   /* another object of this band is CLEARLY nearer */
        }
        uint8_t pen;
        uint32_t c;
        static int nophoto = -1; if (nophoto < 0) nophoto = getenv("S23_NOPHOTO") != NULL;   /* TEST */
        { static int npx = -1, pxs[8], pys[8]; if (npx < 0) { const char *e = getenv("S23_PIXDBG"); npx = 0;   /* debug: who writes pixel x,y[;x,y...] and from where */
              while (e && npx < 8 && sscanf(e, "%d,%d", &pxs[npx], &pys[npx]) == 2) { npx++; e = strchr(e, ';'); if (e) e++; } }
          extern uint32_t rr_frame; static long pf = -2; if (pf == -2) { const char *f = getenv("S23_PIXFROM"); pf = f ? atol(f) : 0; }   /* $S23_PIXFROM=frame: from there only */
          if ((long)rr_frame >= pf) for (int j = 0; j < npx; j++) if (vx == pxs[j] && oy == pys[j]) {
              uint8_t pn; const uint32_t cc = tex_lookup(pbase, penshift, penmask, tx, ty + (uint32_t)p->tbase, &pn);
              fprintf(stderr, "[PIX] f%u %d,%d entry %d model %d tx %X ty %X tbase %X shade %d zkey %07X pen %02X pbase %X rgb %06X\n", rr_frame, vx, oy, p->entry, p->model, tx, ty, p->tbase, (int)(i * ooz), p->zkey, pn, pbase, cc & 0xFFFFFF); } }
        if (!nophoto && s23_game.photo_page && (uint32_t)(ty + (uint32_t)p->tbase - (uint32_t)s23_game.photo_page) < 256u && tx < (uint32_t)S23_PHOTO_W)   /* the camera board's page (any way a polygon reaches it) */
            { c = s23_photo_tex[((ty + (uint32_t)p->tbase - (uint32_t)s23_game.photo_page) & 255) * S23_PHOTO_W + (tx & (S23_PHOTO_W - 1))]; pen = 1; }
        else { ty += (uint32_t)p->tbase; c = tex_lookup(pbase, penshift, penmask, tx, ty, &pen); }
        int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
        int sh = (int)(i * ooz); if (sh < 0) sh = 0; if (sh > (p->boost ? 127 : 63)) sh = p->boost ? 127 : 63;
        r = r * sh >> 6; g = g * sh >> 6; b = b * sh >> 6; if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        if (rx.fog && p->fog) {                          /* THE DEPTH FOG: level = the PCZ table at depth (z * 64 = zsort >> 8), toward the C404 fog colour */
            int cz = (int)(ooz * 64.0f); if (cz < 0) cz = 0; if (cz > 0x1FFF) cz = 0x1FFF;
            const int ff = rx.fogtab[cz];
            if (ff) { r = (r * (256 - ff) + rx.cr * ff) >> 8; g = (g * (256 - ff) + rx.cg * ff) >> 8; b = (b * (256 - ff) + rx.cb * ff) >> 8; }
        }
        if (rx.pfade) { r = r * rx.pr >> 8; g = g * rx.pg >> 8; b = b * rx.pb >> 8; }
        if (rx.fadefactor != 0xFF) { r = (r * rx.fadefactor + rx.fr * rx.fadefactor_inv) >> 8; g = (g * rx.fadefactor + rx.fg * rx.fadefactor_inv) >> 8; b = (b * rx.fadefactor + rx.fb * rx.fadefactor_inv) >> 8; }
        const int use_alpha = rx.alpha != 0xFF && (p->alpha_enabled || pen == rx.alpha_pen);
        if (use_alpha || p->blend) {
            const uint32_t dc = dest[vx]; const int dr = dc >> 16 & 0xFF, dg = dc >> 8 & 0xFF, db = dc & 0xFF;
            if (use_alpha) { r = (r * rx.alpha + dr * rx.alpha_inv) >> 8; g = (g * rx.alpha + dg * rx.alpha_inv) >> 8; b = (b * rx.alpha + db * rx.alpha_inv) >> 8; }
            else { r = (r + dr) >> 1; g = (g + dg) >> 1; b = (b + db) >> 1; }
        }
        dest[vx] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        pm[vx] = (uint8_t)((pm[vx] & ~1) | p->prioverchar);
    }
}
/* a triangle, scan-converted at pixel centres inside the scissor 0..639 x 0..479 (MAME's render_triangle), parameters linear in screen space */
static void triangle(const poly *p, const pvert *a, const pvert *b, const pvert *c)
{
    const pvert *v[3] = { a, b, c };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (v[j]->y > v[j + 1]->y) { const pvert *t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
    const float area = (v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[2]->x - v[0]->x) * (v[1]->y - v[0]->y);
    if (area == 0) return;
    float dpdx[4], dpdy[4];
    for (int q = 0; q < 4; q++) {
        const float p0 = v[0]->p[q], p1 = v[1]->p[q], p2 = v[2]->p[q];
        dpdx[q] = ((p1 - p0) * (v[2]->y - v[0]->y) - (p2 - p0) * (v[1]->y - v[0]->y)) / area;
        dpdy[q] = ((p2 - p0) * (v[1]->x - v[0]->x) - (p1 - p0) * (v[2]->x - v[0]->x)) / area;
    }
    int y0 = (int)ceilf(v[0]->y - 0.5f), y1 = (int)ceilf(v[2]->y - 0.5f);
    if (y0 < 0) y0 = 0; if (y1 > 480) y1 = 480;
    for (int y = y0; y < y1; y++) {
        const float yc = (float)y + 0.5f;
        const float xl0 = v[0]->x + (v[2]->x - v[0]->x) * (yc - v[0]->y) / (v[2]->y - v[0]->y);   /* the long edge */
        float xs;
        if (yc < v[1]->y) xs = v[1]->y == v[0]->y ? v[1]->x : v[0]->x + (v[1]->x - v[0]->x) * (yc - v[0]->y) / (v[1]->y - v[0]->y);
        else xs = v[2]->y == v[1]->y ? v[1]->x : v[1]->x + (v[2]->x - v[1]->x) * (yc - v[1]->y) / (v[2]->y - v[1]->y);
        float xa = xl0 < xs ? xl0 : xs, xb = xl0 < xs ? xs : xl0;
        int x0 = (int)ceilf(xa - 0.5f), x1 = (int)ceilf(xb - 0.5f);
        if (x0 < 0) x0 = 0; if (x1 > 640) x1 = 640;
        if (x0 >= x1) continue;
        float s[4];
        for (int q = 0; q < 4; q++) s[q] = v[0]->p[q] + dpdx[q] * ((float)x0 + 0.5f - v[0]->x) + dpdy[q] * (yc - v[0]->y);
        span(p, y, x0, x1, s, dpdx);
    }
}
static const s23_built *SORTB;
static int cmp_poly(const void *a, const void *b)
{
    const poly *p1 = &SORTB->polys[*(const uint32_t *)a], *p2 = &SORTB->polys[*(const uint32_t *)b];
    if (p1->zkey < p2->zkey) return 1;
    if (p1->zkey > p2->zkey) return -1;
    return p1->index < p2->index ? -1 : p1->index > p2->index ? 1 : 0;
}

/* THE GUN SHOT FLASH: on the frame after an accepted shot the game whitens the whole screen so the cabinet gun's sensor can find
 * the screen -- the C404's screen fade straight to white at full strength (fade FF FF FF, factor FF, flags 03) for ONE frame, from no
 * fade (measured at frame 4802 of a scripted shot; the same as Time Crisis 1). Our gun is the pointer and needs none: with Controls >
 * Gun shot flash OFF (gun_flash=0 in tc2.cfg, or ENG_GUN_FLASH=0) such an instant white is not drawn for up to 2 frames. A real fade
 * to white ramps (the factor climbs over frames) and is left alone. The game itself is untouched: only the frame's copy changes. */
static volatile int gun_flash = 100;                    /* the flash's strength, percent: 100 = as the arcade, 0 = none (Controls > Gun shot flash; F6 cycles) */
void ss22_gl_set_gun_flash(int pct) { gun_flash = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
static void gun_flash_filter(s23_frame_state *fs)
{
    static int prev_fade, run, env = -1;
    if (env < 0) { const char *e = getenv("ENG_GUN_FLASH"); env = e != NULL; if (e) gun_flash = atoi(e) <= 0 ? 0 : atoi(e) > 100 ? 100 : atoi(e) == 1 ? 100 : atoi(e); }
    uint8_t *r = fs->c404;                               /* 8-bit registers in the low bytes: fade RGB 0x16-0x18, factor 0x19, flags 0x1A */
    const int fade = (r[2 * 0x1A + 1] & 3) && r[2 * 0x19 + 1];
    const int white = fade && r[2 * 0x19 + 1] >= 0xF0 && r[2 * 0x16 + 1] >= 0xF0 && r[2 * 0x17 + 1] >= 0xF0 && r[2 * 0x18 + 1] >= 0xF0;
    if (!white) run = 0;
    else if (run || !prev_fade) run++;                   /* white straight out of no fade: a flash (a ramp reaching white has prev_fade set) */
    prev_fade = fade;
    if (gun_flash < 100 && run && run <= 2) r[2 * 0x19 + 1] = (uint8_t)(r[2 * 0x19 + 1] * gun_flash * gun_flash / 10000);   /* the white fade's factor, squared: the fade's brightness goes with its square root (measured: factor 25/50/75 % = 49/69/86 % of the flash), so "50 %" is a flash HALF as bright */
}

/* THE Z-FIGHT DETECTOR ($S23_ZFIGHT=max_dz, e.g. 64): the hardware sorts whole polygons (no depth buffer), so the analogue of z-fighting
 * is two polygons of DIFFERENT render-list entries whose sort keys nearly tie while they overlap on screen -- a small move swaps which is
 * drawn on top and the picture flickers. Each such pair (on screen, overlap >= 25% of the smaller, |dz| <= max_dz, same absolute
 * priority band) is logged once a frame with its entries and models; a pair whose order differs from the previous frame is marked FLIP. */
static void zfight_report(const s23_built *b)
{
    static int maxdz = -2; static unsigned long fn;
    if (maxdz == -2) { const char *e = getenv("S23_ZFIGHT"); maxdz = e ? atoi(e) : -1; }
    if (maxdz < 0) return;
    fn++;
    static uint64_t prev[4096], cur[4096]; static int nprev; int ncur = 0;
    for (int i = 0; i < b->npoly; i++) {
        const poly *p = &b->polys[b->order[i]];
        float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
        const int pcl = (320 - p->vp_size_x) + p->vp_offset_x, pct = (240 - p->vp_size_y) - p->vp_offset_y;
        for (int k = 0; k < p->n; k++) { const float x = p->pv[k].x + pcl, y = p->pv[k].y + pct; if (x < ax0) ax0 = x; if (x > ax1) ax1 = x; if (y < ay0) ay0 = y; if (y > ay1) ay1 = y; }
        if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0; if (ax1 > 640) ax1 = 640; if (ay1 > 480) ay1 = 480;
        if (ax1 - ax0 < 2 || ay1 - ay0 < 2) continue;
        for (int j = i + 1; j < b->npoly && j < i + 64; j++) {
            const poly *q = &b->polys[b->order[j]];
            if ((p->zkey >> 21) != (q->zkey >> 21)) break;
            const int dz = (int)(p->zkey & 0x1FFFFF) - (int)(q->zkey & 0x1FFFFF);
            if (dz > maxdz) break;                         /* sorted far to near: the rest are further apart */
            if (q->entry == p->entry) continue;
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            const int qcl = (320 - q->vp_size_x) + q->vp_offset_x, qct = (240 - q->vp_size_y) - q->vp_offset_y;
            for (int k = 0; k < q->n; k++) { const float x = q->pv[k].x + qcl, y = q->pv[k].y + qct; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
            if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > 640) bx1 = 640; if (by1 > 480) by1 = 480;
            if (bx1 - bx0 < 2 || by1 - by0 < 2) continue;
            const float ox = (ax1 < bx1 ? ax1 : bx1) - (ax0 > bx0 ? ax0 : bx0), oy = (ay1 < by1 ? ay1 : by1) - (ay0 > by0 ? ay0 : by0);
            if (ox <= 0 || oy <= 0) continue;
            const float aa = (ax1 - ax0) * (ay1 - ay0), ab = (bx1 - bx0) * (by1 - by0), ov = ox * oy / (aa < ab ? aa : ab);
            if (ov < 0.25f) continue;
            /* the pair by (model, model) in the order drawn: the same pair drawn the other way round last frame = a FLIP */
            const uint32_t ma = (uint32_t)(p->model & 0xFFFF) | (uint32_t)(p->entry & 0xFFF) << 16, mb = (uint32_t)(q->model & 0xFFFF) | (uint32_t)(q->entry & 0xFFF) << 16;
            const uint64_t key = (uint64_t)ma << 32 | mb, rev = (uint64_t)mb << 32 | ma;
            int flip = 0; for (int k = 0; k < nprev && !flip; k++) flip = prev[k] == rev;
            if (ncur < 4096) cur[ncur++] = key;
            fprintf(stderr, "[ZFIGHT] frame %lu: entries %d/%d models %d/%d dz %d overlap %.0f%% box %.0f,%.0f-%.0f,%.0f%s\n", fn, p->entry, q->entry, p->model, q->model,
                    dz, 100 * ov, ox > 0 ? (ax0 > bx0 ? ax0 : bx0) : 0, ay0 > by0 ? ay0 : by0, ax1 < bx1 ? ax1 : bx1, ay1 < by1 ? ay1 : by1, flip ? " FLIP" : "");
        }
    }
    memcpy(prev, cur, sizeof cur[0] * (size_t)ncur); nprev = ncur;
}

/* ---------------------------------------------------------------- building a frame (the geometry stage, shared by both back ends) */
static int building_mid;                               /* build() is making the in-between frame (motion smoothing): no stateful filters or probes */
static void build(s23_built *b)
{
    BLD = b; FS = &b->fs;
    if (!building_mid) gun_flash_filter(&b->fs);          /* the in-between frame is a copy of an already-filtered snapshot */
    const s23_frame_state *fs = &b->fs;
    GFX.pr = c404b(0); GFX.pg = c404b(1); GFX.pb = c404b(2); GFX.alpha_color = c404b(0xF); GFX.alpha_pen = c404b(0x10); GFX.alpha = c404b(0x11);
    GFX.fr = c404b(0x16); GFX.fg = c404b(0x17); GFX.fb = c404b(0x18); GFX.ffactor = c404b(0x19); GFX.fflags = c404b(0x1A);
    GFX.layers = c404b(0x1F);
    GFX.cr = c404b(5); GFX.cg = c404b(6); GFX.cb = c404b(7); GFX.fog = 0;
    if (fs->cz_on && s23_game.cz_fog) {                   /* the PCZ half -> a depth-indexed level table (Super 22's recalc_czram, engine/fog_hw.c) */
        /* GORgON (MAME gorgon recalc_czram + Super 22's fog_quad): a bank may be REVERSED (level = index ^ 0xFF) and has a level OFFSET */
        uint8_t *t = b->fogtab; const int rev = fs->cz_rev, dl = fs->cz_delta;
        int small_val = 0x2000, small_off = rev, large_val = 0, large_off = rev ^ 0xFF, prev = 0;
        #define LV(f) ((uint8_t)((f) + dl < 0 ? 0 : (f) + dl > 0xFF ? 0xFF : (f) + dl))
        memset(t, 0, 0x2000);
        for (int i = 0; i < 0x100; i++) {
            const int f = i ^ rev;
            int val = fs->pcz[f]; if (val > 0x2000) val = 0x2000;
            if (i > 0) { if (prev >= val) continue; for (int j = prev; j < val; j++) t[j] = LV(f); }
            if (val < small_val) { small_val = val; small_off = f; }
            if (val > large_val) { large_val = val; large_off = f; }
            prev = val;
        }
        for (int j = 0; j < small_val && j < 0x2000; j++) t[j] = LV(small_off);
        for (int j = large_val; j < 0x2000; j++) t[j] = LV(large_off);
        #undef LV
        for (int j = 0; j < 0x2000 && !GFX.fog; j++) GFX.fog = t[j] != 0;
    }
    if (!building_mid) { static long lf = -2, lt; static unsigned long fn; fn++;               /* $S23_C404FRAMES=from:to -- the C404's registers every frame */
      if (lf == -2) { lf = -1; const char *e = getenv("S23_C404FRAMES"); if (e) sscanf(e, "%ld:%ld", &lf, &lt); }
      if (lf >= 0 && (long)fn >= lf && (long)fn <= lt) { fprintf(stderr, "[C404] %lu:", fn); for (int k = 0; k < 0x20; k++) fprintf(stderr, " %02X", c404b(k)); fprintf(stderr, " | list %d\n", fs->list.count); } }
    static int dump = -1; if (dump < 0) dump = getenv("S23_C404DUMP") ? atoi(getenv("S23_C404DUMP")) : 0;
    if (dump && !building_mid) { static unsigned long n; if (++n == (unsigned long)dump) { fprintf(stderr, "C404:"); for (int k = 0; k < 0x20; k++) fprintf(stderr, " %04X", c404(k)); fprintf(stderr, "\n"); } }
    int r = c404b(8), g = c404b(9), bl = c404b(10);
    if ((GFX.fflags & 1) && GFX.ffactor) {
        const int s1 = 0xFF - GFX.ffactor, s2 = 0x100 - s1;
        r = (r * s1 + GFX.fr * s2) >> 8; g = (g * s1 + GFX.fg * s2) >> 8; bl = (bl * s1 + GFX.fb * s2) >> 8;
    }
    GFX.bg = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
    GNPOLY = 0;
    if ((GFX.layers & 1) && roms_ok)
        for (int i = 0; i < fs->list.count; i++) {
            const s23_render_entry *e = &fs->list.e[i];
            if (e->type == S23_RE_MODEL) { render_model(e); s23_render_models++; }
            else if (e->type == S23_RE_IMMEDIATE) { render_immediate(e); s23_render_immediate++; }
            else if (e->type == S23_RE_DIRECT) { render_direct(e); s23_render_direct++; }
        }
    { static int jit = -1; if (jit < 0) { const char *e = getenv("S23_ZJITTER"); jit = e ? atoi(e) : 0; }   /* TEST ONLY: nudge every z key by up
       * to +-jit (a hash of its entry), to find the pixels whose result depends on a near-tie in the draw order (z-fighting candidates) */
      if (jit > 0) for (int i = 0; i < GNPOLY; i++) { poly *q = &GPOLYS[i]; uint32_t h = (uint32_t)q->entry * 2654435761u; h ^= h >> 15;
          int z = (int)(q->zkey & 0x1FFFFF) + (int)(h % (uint32_t)(2 * jit + 1)) - jit; if (z < 0) z = 0; if (z > 0x1FFFFF) z = 0x1FFFFF;
          q->zkey = (q->zkey & ~0x1FFFFFu) | (uint32_t)z; } }
    if (s23_game.depth_role) {                            /* role 2 (the raft's foam) is drawn before every role-1 polygon (the raft): it covers it */
        uint32_t far1 = 0; for (int i = 0; i < GNPOLY; i++) if (GPOLYS[i].role == 1 && GPOLYS[i].zkey > far1) far1 = GPOLYS[i].zkey;
        if (far1) for (int i = 0; i < GNPOLY; i++) if (GPOLYS[i].role == 2 && GPOLYS[i].zkey <= far1) GPOLYS[i].zkey = far1 + 1;
    }
    for (int i = 0; i < GNPOLY; i++) b->order[i] = (uint32_t)i;
    SORTB = b; qsort(b->order, (size_t)GNPOLY, sizeof b->order[0], cmp_poly);
    if (!building_mid) zfight_report(b);
    static int modelseen = -1; if (modelseen < 0) modelseen = getenv("S23_MODELSEEN") != NULL;
    if (!building_mid && modelseen) { static uint8_t seen[8192];   /* $S23_MODELSEEN: each model number the first time it is drawn, with its entry */
      extern uint32_t rr_frame;
      for (int k = 0; k < fs->list.count; k++) { const s23_render_entry *e = &fs->list.e[k];
          if (e->type == S23_RE_MODEL && e->model >= 0 && e->model < 8192 && !seen[e->model]) { seen[e->model] = 1;
              fprintf(stderr, "[SEEN] frame %u entry %d model %d ap %d v %d %d %d m0 %d vp %d,%d\n", rr_frame, k, e->model, e->absolute_priority, e->v[0], e->v[1], e->v[2], e->m[0], e->vp_size_x, e->vp_size_y); } } }
    if (!building_mid) { static long want = -2, built; static int by_frame; if (want == -2) { S23_SS_ENV(want); S23_SS_ENV(by_frame); const char *e = getenv("S23_POLYDUMP"); by_frame = e && *e == 'f'; want = e ? atol(e + by_frame) : -1; }
      extern uint32_t rr_frame; ++built;
      if (by_frame ? (long)rr_frame == want : built == want) {   /* $S23_POLYDUMP=n: every polygon of the n-th built frame in draw order (f<n>: of game frame n) */
          fprintf(stderr, "[C404] alpha_color %02X alpha_pen %02X alpha %02X\n", GFX.alpha_color, GFX.alpha_pen, GFX.alpha);
          for (int k = 0; k < fs->list.count; k++) { const s23_render_entry *e = &fs->list.e[k];
              fprintf(stderr, "[ENTRY] %4d type %d model %5d model2 %5d ap %d v %d %d %d  m %d %d %d / %d %d %d / %d %d %d  scale %.3f  blend %d fov %.1f seg %d zoff %X vpp %X vp %d,%d\n", k, e->type, e->model, e->model2,
                      e->absolute_priority, e->v[0], e->v[1], e->v[2], e->m[0], e->m[1], e->m[2], e->m[3], e->m[4], e->m[5], e->m[6], e->m[7], e->m[8], e->scaling, e->model_blend_factor, e->vp_fov, e->seg, e->zoff, e->vp_prio, e->vp_size_x, e->vp_size_y); }
          for (int k = 0; k < GNPOLY; k++) {
              const poly *q = &GPOLYS[b->order[k]]; float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
              for (int j = 0; j < q->n; j++) { if (q->pv[j].x < x0) x0 = q->pv[j].x; if (q->pv[j].x > x1) x1 = q->pv[j].x; if (q->pv[j].y < y0) y0 = q->pv[j].y; if (q->pv[j].y > y1) y1 = q->pv[j].y; }
              const s23_render_entry *e = &fs->list.e[q->entry];
              fprintf(stderr, "[POLY] %4d entry %4d type %d model %5d  x %6.1f..%6.1f y %6.1f..%6.1f  zkey %07X  tbase %05X cmode %d color %04X poc %d vp %d,%d off %d,%d blend %d alpha %d stencil %d\n",
                      k, q->entry, e->type, q->model, x0, x1, y0, y1, q->zkey, q->tbase, q->cmode, q->color_base, q->prioverchar, q->vp_size_x, q->vp_size_y, q->vp_offset_x, q->vp_offset_y, q->blend, q->alpha_enabled, q->stencil);
              if (s23_game.photo_page && q->tbase == s23_game.photo_page) { fprintf(stderr, "[PHOTOUV]"); for (int j = 0; j < q->n; j++) { const float w = q->pv[j].p[0] != 0 ? q->pv[j].p[0] : 1.f; fprintf(stderr, " %.0f,%.0f", q->pv[j].p[1] / w, q->pv[j].p[2] / w); } fprintf(stderr, "\n"); }
          } } }
    s23_render_polys += (unsigned long)GNPOLY;
    if (GFX.layers & 4) {
        static uint8_t tpri[640 * 480];                  /* the worker's alone */
        memset(b->mix, 0, sizeof b->mix); memset(tpri, 0, sizeof tpri);
        s23_text_layer(fs, b->mix, tpri);
        for (int k = 0; k < 640 * 480; k++) if (tpri[k]) b->mix[k] |= 0x8000; else b->mix[k] = 0;
        for (int y = 0; y < 480; y++) {                 /* the bands: a row opaque everywhere, coloured by its commonest pen */
            const uint16_t *row = &b->mix[y * 640];
            int full = 1; for (int x = 0; x < 640 && full; x++) full = (row[x] & 0x8000) != 0;
            b->band[y] = 0;
            if (!full) continue;
            uint16_t best = row[0]; int bestn = 0;
            for (int x = 0; x < 640; x += 8) { int n = 0; for (int x2 = 0; x2 < 640; x2 += 8) n += row[x2] == row[x]; if (n > bestn) { bestn = n; best = row[x]; } }
            b->band[y] = best;
        }
    } else memset(b->band, 0, sizeof b->band);
}

/* the software picture of a built frame: background, low text, the polygons far to near, the text the polygons let through */
/* GORgON's SPRITES (MAME gorgon_state::render_sprite_tile + render_sprite_scanline): each tile a 32x32 8-bit picture (pen 0xFF clear) in
 * palette bank color*256, stretched to xs x ys pixels; the C404's screen fade on when its flag 2 or the sprite's own fade bit; the sprite's
 * own alpha unless its bank is the C404's opaque bank and the pen isn't its alpha pen. Drawn into an ARGB overlay the back end lays over the
 * polygons (ponytail: in list order, over every polygon -- MAME sorts them by depth among the polygons; sort them in if one shows through wrong) */
int s23_spr_fullpic, s23_spr_hud;
int s23_spr_only_fe;                                     /* 1: draw only the sprites over the text layer (cz 0xFE, s23_game.spr_fe_over) */
/* the "Effects" option (eng_display fx_filter): one tile of an effect sprite as a 34 x 34 premultiplied cell (0xAARRGGBB, GL_BGRA), its
 * 32 x 32 texels in the middle and a 1-texel rim from the neighbouring tiles of the same sprite (clear past the sprite's edge), so a
 * filtered tile blends into its neighbours with no seam and fades out at the sprite's own edge. The colour as s23_sprites_draw's */
void s23_spr_cell(const s23_frame_state *fs, const s23_sprite *s, int row, int col, uint32_t *cell)
{
    const uint8_t *c4 = fs->c404;
    const int fflags = c4[2 * 0x1A + 1], ffac = 0xFF - c4[2 * 0x19 + 1], fr = c4[2 * 0x16 + 1], fg = c4[2 * 0x17 + 1], fb = c4[2 * 0x18 + 1];
    const int acolor = c4[2 * 0x0F + 1], apen = c4[2 * 0x10 + 1];
    const uint32_t *pal = &fs->pens[(s->color & 0x7F) << 8];
    const int fade = (fflags & 2) || s->fade, af = 0xFF - s->alpha, ablend = (s->color & 0x7F) != acolor;
    for (int cy = 0; cy < 34; cy++) for (int cx = 0; cx < 34; cx++) {
        int r = row, c = col, v = cy - 1, u = cx - 1;
        if (v < 0) { r--; v = 31; } else if (v > 31) { r++; v = 0; }
        if (u < 0) { c--; u = 31; } else if (u > 31) { c++; u = 0; }
        uint32_t o = 0;
        if (r >= 0 && r < s->rows && c >= 0 && c < s->cols) {
            const uint8_t *src = sprrom + (((uint32_t)s->code + r * s->cols + c) & 0x3FFF) * 1024u;
            const int pen = src[v * 32 + u];
            if (pen != 0xFF) {
                const uint32_t pc = pal[pen]; int cr = pc >> 16 & 0xFF, cg = pc >> 8 & 0xFF, cb = pc & 0xFF;
                if (fade && ffac != 0xFF) { cr = (cr * ffac + fr * (0x100 - ffac)) >> 8; cg = (cg * ffac + fg * (0x100 - ffac)) >> 8; cb = (cb * ffac + fb * (0x100 - ffac)) >> 8; }
                const uint32_t a = af != 0xFF && (ablend || (s23_game.spr_alpha ? pen >= apen : pen == apen)) ? (uint32_t)af : 0xFF;
                o = a << 24 | (cr * a / 255) << 16 | (cg * a / 255) << 8 | (cb * a / 255);
            }
        }
        cell[cy * 34 + cx] = o;
    }
}
void s23_sprites_draw(const s23_frame_state *fs, int i0, int i1, uint32_t *ovl, int ow, int ox, int bb[4])
{
    const uint8_t *c4 = fs->c404;
    const int fflags = c4[2 * 0x1A + 1], ffac = 0xFF - c4[2 * 0x19 + 1], fr = c4[2 * 0x16 + 1], fg = c4[2 * 0x17 + 1], fb = c4[2 * 0x18 + 1];
    const int acolor = c4[2 * 0x0F + 1], apen = c4[2 * 0x10 + 1];
    bb[0] = ow; bb[1] = 480; bb[2] = bb[3] = 0;
    for (int i = i0; i < i1 && i < fs->nspr; i++) {
        const s23_sprite *s = &fs->spr[i];
		if (s23_spr_only_fe && s->cz != 0xFE) continue;
        const uint32_t *pal = &fs->pens[(s->color & 0x7F) << 8];
        const int fade = (fflags & 2) || s->fade, af = 0xFF - s->alpha, ablend = (s->color & 0x7F) != acolor;
        if (s23_fx_smooth() && s23_spr_is_fx(s)) {   /* "Effects": filtered, from the rimmed cells (the GPU draws them as filtered quads instead: src/s23_gl.c) */
            static uint32_t cell[34 * 34];
            for (int row = 0; row < s->rows; row++) for (int col = 0; col < s->cols; col++) {
                s23_spr_cell(fs, s, row, col, cell);
                const int x0 = s->x + col * s->xs, y0 = s->y + row * s->ys, ax = abs(s->xs), ay = abs(s->ys);
                if (!ax || !ay) continue;
                const int xl = s->xs < 0 ? x0 + s->xs : x0, yt = s->ys < 0 ? y0 + s->ys : y0;
                for (int y = yt < 0 ? 0 : yt; y < yt + ay && y < 480; y++) {
                    const float tv = (y + 0.5f - y0) * 32.f / s->ys + 0.5f; const int iv = (int)floorf(tv); const float fv = tv - iv;
                    for (int x = xl < -ox ? -ox : xl; x < xl + ax && x + ox < ow; x++) {
                        const float tu = (x + 0.5f - x0) * 32.f / s->xs + 0.5f; const int iu = (int)floorf(tu); const float fu = tu - iu;
                        if (iu < 0 || iu > 32 || iv < 0 || iv > 32) continue;
                        const uint32_t *t = &cell[iv * 34 + iu];
                        float ch[4];
                        for (int k = 0; k < 4; k++) { const int sh = 8 * k;
                            ch[k] = ((t[0] >> sh & 0xFF) * (1 - fu) + (t[1] >> sh & 0xFF) * fu) * (1 - fv) + ((t[34] >> sh & 0xFF) * (1 - fu) + (t[35] >> sh & 0xFF) * fu) * fv; }
                        const uint32_t a = (uint32_t)(ch[3] + 0.5f); if (!a) continue;
                        uint32_t o = a << 24;
                        for (int k = 0; k < 3; k++) { uint32_t q = (uint32_t)(ch[k] * 255.f / ch[3] + 0.5f); o |= (q > 255 ? 255 : q) << (8 * k); }
                        ovl[y * ow + x + ox] = o;
                        if (x + ox < bb[0]) bb[0] = x + ox; if (x + ox >= bb[2]) bb[2] = x + ox + 1; if (y < bb[1]) bb[1] = y; if (y >= bb[3]) bb[3] = y + 1;
                    }
                }
            }
            continue;
        }
        for (int row = 0; row < s->rows; row++) for (int col = 0; col < s->cols; col++) {
            const uint8_t *src = sprrom + (((uint32_t)s->code + row * s->cols + col) & 0x3FFF) * 1024u;
            const int x0 = s->x + col * s->xs, y0 = s->y + row * s->ys, ax = abs(s->xs), ay = abs(s->ys);
            const int xl = s->xs < 0 ? x0 + s->xs : x0, yt = s->ys < 0 ? y0 + s->ys : y0;
            for (int y = yt < 0 ? 0 : yt; y < yt + ay && y < 480; y++) {
                int v = (y - yt) * 32 / ay; if (s->ys < 0) v = 31 - v;
                for (int x = xl < -ox ? -ox : xl; x < xl + ax && x + ox < ow; x++) {
                    int u = (x - xl) * 32 / ax; if (s->xs < 0) u = 31 - u;
                    const int pen = src[v * 32 + u]; if (pen == 0xFF) continue;
                    const uint32_t c = pal[pen]; int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
                    if (fade && ffac != 0xFF) { r = (r * ffac + fr * (0x100 - ffac)) >> 8; g = (g * ffac + fg * (0x100 - ffac)) >> 8; b = (b * ffac + fb * (0x100 - ffac)) >> 8; }
                    const uint32_t a = af != 0xFF && (ablend || (s23_game.spr_alpha ? pen >= apen : pen == apen)) ? (uint32_t)af : 0xFF;
                    ovl[y * ow + x + ox] = a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                    if (x + ox < bb[0]) bb[0] = x + ox; if (x + ox >= bb[2]) bb[2] = x + ox + 1; if (y < bb[1]) bb[1] = y; if (y >= bb[3]) bb[3] = y + 1;
                }
            }
        }
    }
}

/* sprites i0..i1-1 composited over the picture (the software renderer; the GPU's: src/s23_gl.c spr_group) */
static void soft_sprites(const s23_built *b, int i0, int i1, uint32_t *rgba)
{
    static uint32_t ovl[640 * 480]; int bb[4];
    if (i0 >= i1) return;
    s23_sprites_draw(&b->fs, i0, i1, ovl, 640, 0, bb);
    for (int y = bb[1]; y < bb[3]; y++) for (int x = bb[0]; x < bb[2]; x++) { const int k = y * 640 + x; const uint32_t s = ovl[k], a = s >> 24; ovl[k] = 0; if (!a) continue;
        prio[k] = (uint8_t)((prio[k] & ~1) | 2);   /* the text layer goes over sprites as over polygons (session 15: "Congratulations!" under the sun; MAME leaves it under) */
        if (a == 0xFF) { rgba[k] = s & 0xFFFFFF; continue; }
        const uint32_t d = rgba[k]; uint32_t o = 0;
        for (int sh = 0; sh <= 16; sh += 8) o |= (((s >> sh & 0xFF) * a + (d >> sh & 0xFF) * (0x100 - a)) >> 8) << sh;
        rgba[k] = o; }
	 if (s23_game.spr_fe_over) {   /* sprites marked over the text layer (cz 0xFE, MAME's prioverchar): no text drawn again over them */
        s23_spr_only_fe = 1; s23_sprites_draw(&b->fs, i0, i1, ovl, 640, 0, bb); s23_spr_only_fe = 0;
        for (int y = bb[1]; y < bb[3]; y++) for (int x = bb[0]; x < bb[2]; x++) { const int k = y * 640 + x; if (ovl[k] >> 24) prio[k] &= (uint8_t)~2; ovl[k] = 0; } }
}
void s23_soft_raster(const s23_built *b, uint32_t *rgba)
{
    static uint16_t mix[640 * 480]; static uint8_t pri[640 * 480];
    FS = &b->fs;
    const s23_fx *f = &b->fx;
    rx.fade = (f->fflags & 1) != 0; rx.fadefactor = 0xFF; rx.fadefactor_inv = 1;
    if (rx.fade) { rx.fadefactor = 0xFF - f->ffactor; rx.fadefactor_inv = 0x100 - rx.fadefactor; rx.fr = f->fr; rx.fg = f->fg; rx.fb = f->fb; }
    rx.pfade = f->pr || f->pg || f->pb; rx.pr = f->pr; rx.pg = f->pg; rx.pb = f->pb;
    rx.alpha = 0xFF - f->alpha; rx.alpha_inv = 0x100 - rx.alpha; rx.alpha_pen = f->alpha_pen;
    { static long a = -2, z; if (a == -2) { const char *e = getenv("S23_FXLOG"); a = -1; if (e) sscanf(e, "%ld:%ld", &a, &z); }   /* debug: $S23_FXLOG=a:b -- the C404 effects per frame */
      extern uint32_t rr_frame;
      if (a >= 0 && (long)rr_frame >= a && (long)rr_frame <= z) fprintf(stderr, "[FX] f%u fflags %X ffactor %02X fade %02X%02X%02X pfade %02X%02X%02X bg %06X layers %X alpha %02X pen %X\n",
          rr_frame, f->fflags, f->ffactor, f->fr, f->fg, f->fb, f->pr, f->pg, f->pb, (unsigned)f->bg, f->layers, f->alpha, f->alpha_pen);
      static long pal = -2; if (pal == -2) { const char *e = getenv("S23_FXPAL"); pal = e ? strtol(e, NULL, 16) : -1; }   /* + 16 pens of palette <hex> */
      if (a >= 0 && pal >= 0 && (long)rr_frame >= a && (long)rr_frame <= z) { fprintf(stderr, "[PAL] f%u %04lX:", rr_frame, pal << 8);
          for (int k = 0; k < 256; k += getenv("S23_FXPALALL") ? 1 : 16) fprintf(stderr, " %06X", (unsigned)(b->fs.pens[((pal << 8) + k) & 0x7FFF] & 0xFFFFFF)); fprintf(stderr, "\n"); } }
    { static long tf = -2; static unsigned tu, tv, tw, th, tp; if (tf == -2) { const char *e = getenv("S23_TEXDUMP"); tf = -1;   /* debug: $S23_TEXDUMP=frame:u,v,w,h,pal -- texture area (8-bit pens) as texdump.ppm */
          if (e && sscanf(e, "%ld:%x,%x,%x,%x,%x", &tf, &tu, &tv, &tw, &th, &tp) != 6) tf = -1; }
      extern uint32_t rr_frame;
      if (tf >= 0 && (long)rr_frame == tf) { FS = &b->fs; FILE *o = fopen("texdump.ppm", "wb");
          if (o) { fprintf(o, "P6\n%u %u\n255\n", tw, th); for (unsigned y = 0; y < th; y++) for (unsigned x = 0; x < tw; x++) { uint8_t pn; const uint32_t c = tex_lookup(tp << 8, 0, 0xFF, tu + x, tv + y, &pn); const uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c }; fwrite(px, 1, 3, o); } fclose(o); } } }
    rx.fog = f->fog; rx.cr = f->cr; rx.cg = f->cg; rx.cb = f->cb; rx.fogtab = b->fogtab;
    if (seamtest < 0) { const char *e = getenv("S23_SEAMTEST"); seamtest = e ? atoi(e) : 0; }
    for (int k = 0; k < 640 * 480; k++) rgba[k] = seamtest ? 0xFF00FF : f->bg;
    memset(pri, 0, sizeof pri);
    if ((f->layers & 4) && !seamtest) {
        for (int k = 0; k < 640 * 480; k++) { mix[k] = b->mix[k] & 0x7FFF; pri[k] = (b->mix[k] & 0x8000) ? 4 : 0; }
        s23_text_mix(&b->fs, rgba, mix, pri, 4);
        { static long tf = -2; static int tx, ty; extern uint32_t rr_frame; if (tf == -2) { const char *e = getenv("S23_TEXTPEN"); tf = -1; if (e && sscanf(e, "%ld:%d,%d", &tf, &tx, &ty) != 3) tf = -1; }   /* debug: $S23_TEXTPEN=frame:x,y */
          if (tf >= 0 && (long)rr_frame >= tf && (long)rr_frame < tf + 3) fprintf(stderr, "[TPEN] f%u %d,%d mix %04X pri %d rgb %06X\n", rr_frame, tx, ty, b->mix[ty * 640 + tx], pri[ty * 640 + tx], (unsigned)(rgba[ty * 640 + tx] & 0xFFFFFF)); }
    }
    fb = rgba; prio = pri;
    s23_depth_on();
    if ((s23_depth_mode || s23_game.depth_within) && !seamtest) {                      /* the pre-pass: the nearest OPAQUE pixel everywhere (the GPU does the same: src/s23_gl.c) */
        memset(eb, 0, sizeof eb); for (int k = 0; k < 640 * 480; k++) zb[k] = 0.0f;
        near_pass = 1;
        for (int i = 0; i < b->npoly; i++) {
            const poly *p = &b->polys[b->order[i]];
            if (p->blend || (rx.alpha != 0xFF && p->alpha_enabled)) continue;
            for (int k = 1; k + 1 < p->n; k++) triangle(p, &p->pv[0], &p->pv[k], &p->pv[k + 1]);
        }
        near_pass = 0;
    }
    int sp = 0;                                             /* GORgON's sprites, sorted far to near (s23_render_frame_end), go in among the polygons */
    for (int i = 0; i < b->npoly; i++) {
        const poly *p = &b->polys[b->order[i]];
        if (!seamtest && sp < b->fs.nspr && b->fs.spr[sp].z > (p->zkey & 0x1FFFFF)) { int e = sp; while (e < b->fs.nspr && b->fs.spr[e].z > (p->zkey & 0x1FFFFF)) e++; soft_sprites(b, sp, e, rgba); sp = e; }
        for (int k = 1; k + 1 < p->n; k++) triangle(p, &p->pv[0], &p->pv[k], &p->pv[k + 1]);   /* a fan */
    }
    if (seamtest) return;                                   /* no text, no gamma */
    soft_sprites(b, sp, b->fs.nspr, rgba);                  /* the nearest: over the polygons and under the text */
    if (f->layers & 4) {
        for (int k = 0; k < 640 * 480; k++) if (pri[k] == 6 && !(b->mix[k] & 0x8000)) pri[k] = 0;   /* a "6" with no text pixel under it draws nothing */
        s23_text_mix(&b->fs, rgba, mix, pri, 6);
    }
    for (int i = 0; i < b->fs.list.nrect && i < S23_RECTS; i++) {   /* a game's own rectangles (s23_game.list_hook) */
        const s23_rect *r = &b->fs.list.rect[i];
        for (int y = r->y0 < 0 ? 0 : r->y0; y < r->y1 && y < 480; y++) for (int x = r->x0 < 0 ? 0 : r->x0; x < r->x1 && x < 640; x++) rgba[y * 640 + x] = r->rgb;
    }
    for (int m = 0; m < b->fs.list.nmask && m < S23_MASKS; m++) {   /* and its captions (blended by coverage) */
        const s23_mask *k = &b->fs.list.mask[m];
        for (int y = 0; y < k->h; y++) for (int x = 0; x < k->w; x++) {
            const int sx = k->x + x, sy = k->y + y, a = k->a[y * k->w + x]; if (!a || sx < 0 || sx >= 640 || sy < 0 || sy >= 480) continue;
            uint32_t *d = &rgba[sy * 640 + sx]; uint32_t o = 0;
            for (int sh = 0; sh <= 16; sh += 8) o |= (uint32_t)(((int)(*d >> sh & 0xFF) * (255 - a) + (int)(k->rgb >> sh & 0xFF) * a) / 255) << sh;
            *d = o; }
    }
    if (b->fs.list.img_on) {                              /* and its picture */
        const s23_render_list *L = &b->fs.list; const int w = L->img_x1 - L->img_x0, h = L->img_y1 - L->img_y0;
        if (w > 0 && h > 0)
            for (int y = L->img_y0 < 0 ? 0 : L->img_y0; y < L->img_y1 && y < 480; y++) for (int x = L->img_x0 < 0 ? 0 : L->img_x0; x < L->img_x1 && x < 640; x++)
                rgba[y * 640 + x] = L->img[((y - L->img_y0) * 128 / h) * 128 + (x - L->img_x0) * 128 / w];
    }
    const uint8_t (*gm)[256] = b->fs.gamma;                /* the C404's gamma, last */
    for (int k = 0; k < 640 * 480; k++) { const uint32_t c = rgba[k]; rgba[k] = (uint32_t)gm[0][c >> 16 & 0xFF] << 16 | (uint32_t)gm[1][c >> 8 & 0xFF] << 8 | gm[2][c & 0xFF]; }
}

/* THE WORKER: the frame is BUILT on its own thread from a snapshot taken at the start of vertical blank, while the CPUs run the next frame
 * (and, with no GPU, rasterised there too); the window shows the newest finished picture (one frame behind, as the hardware's frame buffer
 * is), a screenshot waits for its own */
static s23_built *B[2]; static int front;           /* B[front] = the newest finished frame */
static uint32_t frames[2][640 * 480];
static pthread_t worker; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int busy, started, have_frame, have_mid;
static volatile int smooth_n = 1;                      /* in-between frames a game frame: 1 (at 1/2) below 150 pictures/s, 2 (1/3, 2/3) from 150 */
static int mid_n[2];                                   /* how many MIDS[slot] hold */
static s23_built *MIDS[2][2];                             /* motion smoothing: MIDS[front] = halfway from the frame before B[front] to B[front] (the
                                                        * worker builds MIDS[!front] while the window may still draw MIDS[front]); [k] = fraction k */
static s23_built *MID;                                 /* the one being built */
__attribute__((constructor)) static void worker_keep(void)   /* the worker thread's own state is this process's: a capture taken while it was busy
                                                   * (Panic Park's) restored busy = 1 and the first frame waited for it for ever */
{ S23_SS_KEEP(B); S23_SS_KEEP(front); S23_SS_KEEP(worker); S23_SS_KEEP(mu); S23_SS_KEEP(cv); S23_SS_KEEP(busy); S23_SS_KEEP(started); S23_SS_KEEP(have_frame); S23_SS_KEEP(have_mid); S23_SS_KEEP(mid_n); S23_SS_KEEP(MIDS); }

/* ---- MOTION SMOOTHING (Display > Motion smoothing, off by default; motion_smooth in the cfg, $S23_SMOOTH=0/1 for tests) -------------------
 * The board makes 59.906 pictures a second; at 120 Hz and up the window can show one MORE picture between two of them. It is made here, not
 * by the board: a copy of frame N's snapshot whose render-list entries are moved HALFWAY back towards frame N-1 -- each model's view matrix
 * (rows renormalised), translation, scale and morph factor, each immediate polygon's corners -- then built like any frame. The camera is in
 * every entry's matrix, so it moves halfway too. An entry is matched with the same model (or immediate polygon kind) by OCCURRENCE: the
 * k-th entry of a kind in frame N goes with the k-th of that kind in frame N-1. Not moved: an entry with no partner, one that jumped more
 * than a third of its distance or turned more than ~40 degrees in one frame (a new object reusing a slot, a cut), direct quads and the text
 * layer (frame N's). A frame where under half the models found a partner or over a quarter of the partners jumped is a cut: the in-between
 * picture is frame N itself. The cost: frame N is shown half a frame later (the in-between picture goes first). Not the board's picture --
 * nothing here can be checked against MAME. */
int s23_smooth = -1;
int s23_smooth_on(void) { if (s23_game.no_smooth && !getenv("S23_SMOOTH")) return 0;   /* a game whose in-between frames crashed the GPU driver */
    if (s23_smooth < 0) { const char *e = getenv("S23_SMOOTH"); s23_smooth = e ? *e == '1' : 0; } return s23_smooth; }
void s23_smooth_set(int on) { s23_smooth_on(); if (!getenv("S23_SMOOTH")) s23_smooth = on != 0; }
unsigned long s23_smooth_frames, s23_smooth_cuts;      /* in-between frames made; of them, cuts (shown as frame N) */
static uint64_t mid_key(const s23_render_entry *e)
{
    if (e->type == S23_RE_MODEL)
        return (uint64_t)1 << 62 | (uint64_t)e->model << 40 | (uint64_t)e->model2 << 24 | (uint64_t)(e->absolute_priority & 7) << 21 | (uint64_t)(e->mirror_x & 1) << 20 | (uint64_t)(e->transpose & 1) << 19 | (uint64_t)((uint16_t)e->vp_size_x ^ (uint16_t)e->vp_size_y << 5) & 0x7FFFF;
    if (e->type == S23_RE_IMMEDIATE) return (uint64_t)2 << 62 | (uint64_t)(e->imm.type & 0xFFFF) << 40 | (uint64_t)(e->imm.pal & 0xFFFF) << 24 | (e->imm.h & 0xFFFFFF);
    return 0;
}
static double mid_len3(double x, double y, double z) { return sqrt(x * x + y * y + z * z); }
/* entry b halfway back towards a; 0 = too far apart to blend (b is kept as it is) */
static int mid_model(s23_render_entry *o, const s23_render_entry *a, const s23_render_entry *b, double f)
{
    const double dv = mid_len3((double)b->v[0] - a->v[0], (double)b->v[1] - a->v[1], (double)b->v[2] - a->v[2]);
    const double rv = mid_len3(b->v[0], b->v[1], b->v[2]) > mid_len3(a->v[0], a->v[1], a->v[2]) ? mid_len3(b->v[0], b->v[1], b->v[2]) : mid_len3(a->v[0], a->v[1], a->v[2]);
    if (dv > 64.0 && dv > rv / 3.0) return 0;
    double m[9];
    for (int r = 0; r < 3; r++) {
        const double ax = a->m[3 * r], ay = a->m[3 * r + 1], az = a->m[3 * r + 2], bx = b->m[3 * r], by = b->m[3 * r + 1], bz = b->m[3 * r + 2];
        const double la = mid_len3(ax, ay, az), lb = mid_len3(bx, by, bz);
        if (la < 1 || lb < 1) { m[3 * r] = bx; m[3 * r + 1] = by; m[3 * r + 2] = bz; continue; }
        if ((ax * bx + ay * by + az * bz) / (la * lb) < 0.766) return 0;   /* turned more than ~40 degrees */
        double x = ax + (bx - ax) * f, y = ay + (by - ay) * f, z = az + (bz - az) * f; const double l = mid_len3(x, y, z), want = la + (lb - la) * f;
        m[3 * r] = x * want / l; m[3 * r + 1] = y * want / l; m[3 * r + 2] = z * want / l;
    }
    for (int k = 0; k < 9; k++) { double t = m[k] < 0 ? m[k] - 0.5 : m[k] + 0.5; o->m[k] = (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t); }
    for (int k = 0; k < 3; k++) o->v[k] = (int32_t)llround(a->v[k] + ((double)b->v[k] - a->v[k]) * f);
    o->scaling = (float)(a->scaling + (b->scaling - a->scaling) * f);
    o->model_blend_factor = (uint16_t)(int16_t)lround((int16_t)a->model_blend_factor + ((double)(int16_t)b->model_blend_factor - (int16_t)a->model_blend_factor) * f);   /* (signed, above) */
    return 1;
}
static int mid_immediate(s23_render_entry *o, const s23_render_entry *a, const s23_render_entry *b, double f)
{
    const int n = (int)((b->imm.type >> 8) & 0xF) < 4 ? (int)((b->imm.type >> 8) & 0xF) : 4;
    for (int i = 0; i < n; i++) {
        const double ax = (int32_t)a->imm.x[i], ay = (int32_t)a->imm.y[i], az = (int32_t)a->imm.z[i], bx = (int32_t)b->imm.x[i], by = (int32_t)b->imm.y[i], bz = (int32_t)b->imm.z[i];
        const double d = mid_len3(bx - ax, by - ay, bz - az), r = mid_len3(bx, by, bz);
        if (d > 64.0 && d > r / 3.0) return 0;
    }
    for (int i = 0; i < n; i++) {
        o->imm.x[i] = (uint32_t)(int32_t)llround((int32_t)a->imm.x[i] + ((double)(int32_t)b->imm.x[i] - (int32_t)a->imm.x[i]) * f);
        o->imm.y[i] = (uint32_t)(int32_t)llround((int32_t)a->imm.y[i] + ((double)(int32_t)b->imm.y[i] - (int32_t)a->imm.y[i]) * f);
        o->imm.z[i] = (uint32_t)(int32_t)llround((int32_t)a->imm.z[i] + ((double)(int32_t)b->imm.z[i] - (int32_t)a->imm.z[i]) * f);
    }
    return 1;
}
/* the in-between frame of a (frame N-1) and b (frame N) into MID; 0 = none (allocation failed) */
static int build_mid(s23_built **slot, const s23_built *a, const s23_built *b, double f)
{
    if (!*slot && !(*slot = calloc(1, sizeof **slot))) return 0;
    MID = *slot;
    enum { NB = 16384 };
    static int head[NB], tail[NB], nxt[S23_RENDER_MAX]; static uint64_t keys[S23_RENDER_MAX]; static uint8_t used[S23_RENDER_MAX];
    memcpy(&MID->fs, &b->fs, sizeof MID->fs);
    for (int k = 0; k < NB; k++) head[k] = tail[k] = -1;
    const s23_render_list *la = &a->fs.list;
    for (int i = 0; i < la->count; i++) {                 /* frame N-1's entries chained per kind, in list order */
        const uint64_t key = mid_key(&la->e[i]); keys[i] = key; used[i] = 0; nxt[i] = -1;
        if (!key) continue;
        const int h = (int)((key * 0x9E3779B97F4A7C15ull) >> 50) & (NB - 1);
        if (tail[h] < 0) head[h] = i; else nxt[tail[h]] = i;
        tail[h] = i;
    }
    s23_render_list *lo = &MID->fs.list;
    int models = 0, matched = 0, jumped = 0;
    for (int i = 0; i < lo->count; i++) {
        s23_render_entry *o = &lo->e[i];
        const uint64_t key = mid_key(o);
        if (!key) continue;
        if (o->type == S23_RE_MODEL) models++;
        const int h = (int)((key * 0x9E3779B97F4A7C15ull) >> 50) & (NB - 1);
        int j = head[h];
        while (j >= 0 && (used[j] || keys[j] != key)) j = nxt[j];
        if (j < 0) continue;
        used[j] = 1;
        const s23_render_entry *pa = &la->e[j], *pb = &b->fs.list.e[i];
        const int ok = o->type == S23_RE_MODEL ? mid_model(o, pa, pb, f) : mid_immediate(o, pa, pb, f);
        if (o->type == S23_RE_MODEL) { matched++; jumped += !ok; }
    }
    if (f == 0.5 || f < 0.4) s23_smooth_frames++;
    if (models && (matched * 2 < models || jumped * 4 > matched)) {   /* a cut: the in-between picture is frame N */
        if (f == 0.5 || f < 0.4) s23_smooth_cuts++;
        memcpy(&MID->fs.list, &b->fs.list, offsetof(s23_render_list, e) + sizeof b->fs.list.e[0] * (size_t)b->fs.list.count);
    }
    building_mid = 1; build(MID); building_mid = 0;
    { static const char *dir; static long every = -1; static uint32_t pic[640 * 480];   /* $S23_MIDSHOTS=dir:every -- the in-between pictures as PPMs (tests) */
      if (every < 0) { every = 0; const char *e = getenv("S23_MIDSHOTS"); if (e) { static char d[512]; snprintf(d, sizeof d, "%s", e); char *c = strrchr(d, ':'); if (c) { *c = 0; every = atol(c + 1); dir = d; } } }
      extern uint32_t rr_frame;
      if (every > 0 && rr_frame % (uint32_t)every == 0 && (f == 0.5 || f < 0.4)) {
          s23_soft_raster(MID, pic);
          char path[600]; snprintf(path, sizeof path, "%s/m%05u.ppm", dir, rr_frame);
          FILE *f = fopen(path, "wb");
          if (f) { fprintf(f, "P6\n640 480\n255\n"); for (int k = 0; k < 640 * 480; k++) { const uint8_t px[3] = { (uint8_t)(pic[k] >> 16), (uint8_t)(pic[k] >> 8), (uint8_t)pic[k] }; fwrite(px, 1, 3, f); } fclose(f); }
      } }
    return 1;
}
static int soft_raster = 1;                            /* 0 once the window's GPU renderer is up */
void s23_render_soft(int on) { soft_raster = on; }
__attribute__((constructor)) static void soft_keep(void) { S23_SS_KEEP(soft_raster); }   /* (a window's capture loaded headless still draws) */
static void *work(void *u)
{
    (void)u;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!busy) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        build(B[!front]);
        if (soft_raster) s23_soft_raster(B[!front], frames[!front]);
        int mid = 0;
        if (s23_smooth_on() && have_frame) {                /* the in-between frames: 1/2, or 1/3 and 2/3 */
            const int n = smooth_n == 2 ? 2 : 1;
            for (int k = 0; k < n; k++) mid += build_mid(&MIDS[!front][k], B[front], B[!front], n == 1 ? 0.5 : (k + 1) / 3.0);
            if (mid != n) mid = 0;
            mid_n[!front] = mid;
        }
        pthread_mutex_lock(&mu);
        front = !front; have_frame = 1; busy = 0; have_mid = mid;
        pthread_cond_broadcast(&cv);
    }
    return NULL;
}
static void wait_idle(void) { pthread_mutex_lock(&mu); while (busy) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu); }

/* at the start of vertical blank (MAME's screen_update): the other list is drawn, then the lists swap and the drawn one is emptied for the
 * C435 to fill (MAME render_run) */
/* THE PLAYER'S PICTURE ADJUSTMENTS (Display > Arcade gamma / Gamma / Brightness, saved as pic_c404 / pic_gamma / pic_bright in the game's cfg),
 * folded into the final per-channel table both renderers apply last (the GPU's LUT pass, the software one) -- so no shader changes and the
 * same picture either way:  out = clamp(255 * (mix / 255) ^ (1 / gamma) * brightness)  with  mix = i + (C404 table - i) * arcade%.
 * Arcade gamma 100% = the C404 table as the board writes it (our default; MAME applies none = 0%). Defaults leave the table untouched. */
int s23_pic_c404 = 100, s23_pic_gamma = 100, s23_pic_bright = 100;      /* %, gamma x 100, % */
static void picture_adjust(uint8_t g[3][256])
{
    { static int env = -1; if (env < 0) { const char *e = getenv("S23_PIC"); env = e != NULL;   /* tests: $S23_PIC=arcade%,gamma*100,brightness% */
        if (e) sscanf(e, "%d,%d,%d", &s23_pic_c404, &s23_pic_gamma, &s23_pic_bright); } }
    if (s23_pic_c404 == 100 && s23_pic_gamma == 100 && s23_pic_bright == 100) return;
    const double ge = 100.0 / s23_pic_gamma, br = s23_pic_bright / 100.0;
    for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) {
        const double mix = i + (g[ch][i] - i) * s23_pic_c404 / 100.0;
        double v = 255.0 * pow(mix / 255.0, ge) * br;
        g[ch][i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
    }
}
int s23_game_has_view(void) { return s23_game.znear_model > 0.f; }
int s23_game_mounted_gun(void) { return s23_game.mounted_gun; }
int s23_game_no_smooth(void) { return s23_game.no_smooth; }
int s23_game_no_depth(void) { return s23_game.no_depth; }
int s23_game_has_fx(void) { return s23_game.n_spr_fx > 0; }
int s23_fx_smooth(void)
{
    static int ef = -2; if (ef == -2) { const char *e = getenv("S23_FX_FILTER"); ef = e && *e ? atoi(e) != 0 : -1; }
    return s23_game.n_spr_fx > 0 && (ef >= 0 ? ef : g_eng_disp.fx_filter);
}
int s23_spr_is_fx(const s23_sprite *s)
{
    for (int k = 0; k < s23_game.n_spr_fx; k++) if (s->code >= s23_game.spr_fx[k][0] && s->code <= s23_game.spr_fx[k][1]) return 1;
    return 0;
}
void s23_picture_load(void)
{
    int eng_cfg_int(const char *, int);
    s23_pic_c404 = eng_cfg_int("pic_c404", 100);   if (s23_pic_c404 < 0) s23_pic_c404 = 0;     if (s23_pic_c404 > 100) s23_pic_c404 = 100;
    s23_pic_gamma = eng_cfg_int("pic_gamma", 100); if (s23_pic_gamma < 50) s23_pic_gamma = 50;  if (s23_pic_gamma > 200) s23_pic_gamma = 200;
    s23_pic_bright = eng_cfg_int("pic_bright", 100); if (s23_pic_bright < 50) s23_pic_bright = 50; if (s23_pic_bright > 150) s23_pic_bright = 150;
    s23_first_person = eng_cfg_int("first_person", 0) != 0;
}
void s23_render_frame_end(void)
{
    if (!started) { started = 1; B[0] = calloc(1, sizeof *B[0]); B[1] = calloc(1, sizeof *B[1]); pthread_create(&worker, NULL, work, NULL); }
    wait_idle();                                          /* the previous frame is built: the back slot is free */
    s23_frame_state *snap = &B[!front]->fs;
    const uint8_t *c4 = s23_vram_ptr(0x06A08000u), *pal = s23_vram_ptr(0x06A10000u);
    memcpy(snap->c404, c4, sizeof snap->c404);
    { const uint16_t *s23_c412_pcz_live(void); int s23_c412_cz_on(void);   /* the depth fog's table and switch (src/s23_chips.c) */
      memcpy(snap->pcz, s23_c412_pcz_live(), sizeof snap->pcz); snap->cz_on = s23_c412_cz_on(); snap->cz_rev = snap->cz_delta = 0;
      if (s23_game.gorgon) snap->cz_on = s23_game.gorgon->fog(snap->pcz, &snap->cz_rev, &snap->cz_delta); }   /* GORgON: its own CZ RAM */
    { static int on = -1; if (on < 0) { const char *e = getenv("S23_GAMMA"); on = !(e && *e == '0'); }   /* S23_GAMMA=0: as MAME (no gamma) */
      for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) snap->gamma[ch][i] = on ? c4[0x200 + 0x200 * ch + 2 * i + 1] : (uint8_t)i;
      int blank = 1; for (int i = 1; i < 256 && blank; i++) blank = !snap->gamma[0][i];
      if (blank) for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) snap->gamma[ch][i] = (uint8_t)i;   /* before the game loads it */
      picture_adjust(snap->gamma); }
    { static long at = -2; static unsigned long fn; fn++;          /* $S23_C404RAM=frame: the whole 0x800-byte C404 block once */
      if (at == -2) { const char *e = getenv("S23_C404RAM"); at = e ? atol(e) : -1; }
      if (at > 0 && (long)fn == at) { FILE *f = fopen("work/c404ram.bin", "wb"); if (f) { fwrite(c4, 1, 0x800, f); fclose(f); fprintf(stderr, "[C404] frame %lu -> work/c404ram.bin\n", fn); } } }
    for (uint32_t n = 0; n < 0x8000; n++) { const uint32_t o = 2 * n + 1; snap->pens[n] = (uint32_t)pal[o] << 16 | (uint32_t)pal[0x10000 + o] << 8 | pal[0x20000 + o]; }
    memcpy(snap->text, s23_vram_ptr(0x06800000u), sizeof snap->text);
    s23_text_scroll(snap->linexscroll, &snap->yscroll);
	{ void s23_subs_patch(uint8_t *, const uint16_t *, uint16_t); s23_subs_patch(snap->text, snap->linexscroll, snap->yscroll); }   /* Angler King: the English drawn into the captions (src/s23_subs.c) */
    memcpy(snap->sram, s23_c412_sram(), sizeof snap->sram);
    s23_render_list *l = &s23_render[!s23_render_cur];   /* MAME draws the list filled the frame BEFORE (one frame behind) */
    snap->list.count = l->count;
    memcpy(snap->list.e, l->e, sizeof l->e[0] * (size_t)l->count);
    memcpy(snap->list.seg_zoff, l->seg_zoff, sizeof l->seg_zoff);
    snap->list.nrect = 0; snap->list.img_on = 0; snap->list.nmask = 0; snap->list.ntshift = 0; snap->list.ntstretch = 0; snap->list.pillar_fade = 0;
    snap->nspr = s23_game.gorgon && sprrom && (snap->c404[2 * 0x1F + 1] & 2) ? s23_game.gorgon->sprites(snap->spr, 0x280) : 0;   /* C404 layer bit 1: sprites */
    { /* WIDESCREEN SPRITES (Rapid River): a picture covering the 4:3 screen (title, route map) -> s23_spr_fullpic, the game's hud_wide_ok
       * then puts bars at the sides; otherwise the HUD pieces out towards the sides (centre more than 160 from the middle: the timer) move
       * out by the extra width, as the polygon HUD below. ponytail: SOLID sprites (alpha 0) adding up to 60% of the screen = a full picture
       * (the title: six solid 8 x 8-tile blocks; the race's water spray is big see-through sprites from the timer to the bottom) */
      long area = 0;
      for (int i = 0; i < snap->nspr; i++) { const s23_sprite *s = &snap->spr[i]; if (s->alpha == 0) area += (long)abs(s->xs) * s->cols * abs(s->ys) * s->rows; }
      static int run; run = area >= 640L * 480 * 6 / 10 ? run + 1 : 0;
      s23_spr_fullpic = run >= 4;   /* held 4 frames: single-frame bursts in the race (session 11 frames 1665, 1839, 2130) are not a picture */
      extern int g_eng_hud_edges_on; int s23_gl_wide(void); const int ex = s23_gl_wide();
      /* the game says which sprites are its HUD (Rapid River: the timer; moving every sprite near a side broke the route map);
       * s23_spr_hud = the HUD seen in the last 30 frames (play: the game's hud_wide_ok; held over the timer's busy-list gaps) */
      static int since = 1000; int seen = 0;
      if (s23_game.spr_wide) for (int i = 0; i < snap->nspr; i++) { s23_sprite *s = &snap->spr[i]; const int d = s23_game.spr_wide(s);
          if (!d) continue; seen = 1;
          if (ex > 0 && g_eng_hud_edges_on && !s23_spr_fullpic) s->x = (int16_t)(s->x + d * ex); }
      since = seen ? 0 : since + 1; s23_spr_hud = since < 30;
      /* far to near by the C404 depth key (stable: equal keys keep the list order), so the renderers can put each among the polygons as MAME
       * does (the waterfall bonus's sun glow behind the helicopter and the text, the paddles' splash behind the raft) */
	  if (s23_game.spr_first_top) for (int i = 0, j = snap->nspr - 1; i < j; i++, j--) { const s23_sprite t = snap->spr[i]; snap->spr[i] = snap->spr[j]; snap->spr[j] = t; }
      for (int i = 1; i < snap->nspr; i++) { const s23_sprite s = snap->spr[i]; int j = i;
          while (j > 0 && snap->spr[j - 1].z < s.z) { snap->spr[j] = snap->spr[j - 1]; j--; } snap->spr[j] = s; } }
    { static long at = -2, to; extern uint32_t rr_frame; if (at == -2) { const char *e = getenv("S23_SPRLOG"); at = -1; if (e && sscanf(e, "%ld-%ld", &at, &to) < 2) to = at; }   /* debug: $S23_SPRLOG=frame[-frame]: sprites; a range logs the header only */
      if (to > at && (long)rr_frame >= at && (long)rr_frame <= to) { fprintf(stderr, "[SPRN] %u %d %02X full %d", rr_frame, snap->nspr, snap->c404[2 * 0x1F + 1], s23_spr_fullpic);
          for (int i = 0; i < snap->nspr && i < 8; i++) fprintf(stderr, " | %d,%d %04X a%02X z%06X", snap->spr[i].x, snap->spr[i].y, snap->spr[i].code, snap->spr[i].alpha, snap->spr[i].z); fprintf(stderr, "\n"); }
       if (at >= 0 && (long)rr_frame == at) { fprintf(stderr, "[SPR] %u: layer byte %02X, %d sprites, list %d entries; alpha colour %02X pen %02X\n", rr_frame, snap->c404[2 * 0x1F + 1], snap->nspr, snap->list.count, snap->c404[2 * 0x0F + 1], snap->c404[2 * 0x10 + 1]);
        FILE *pf = getenv("S23_PENDUMP") ? fopen(getenv("S23_PENDUMP"), "wb") : NULL;   /* debug: with $S23_SPRLOG, the palette (0x8000 x 0x00RRGGBB) -- a logo from the sprite ROM */
        if (pf) { fwrite(snap->pens, 4, 0x8000, pf); fclose(pf); } }   /* (Gorgon's header words: $S23_SPRRAW) */
      static long at2 = -2; if (at2 == -2) { const char *e = getenv("S23_SPRLOG"), *c = e ? strchr(e, ',') : NULL; at2 = c ? atol(c + 1) : -1; }   /* =a,b: two frames' lists */
      if ((at >= 0 && (long)rr_frame == at) || (long)rr_frame == at2) for (int i = 0; i < snap->nspr; i++) { const s23_sprite *s = &snap->spr[i];
          fprintf(stderr, "[SPR] %u #%d at %d,%d size %d x %d, %d x %d tiles, code %04X colour %02X alpha %02X fade %d z %06X cz %02X\n", rr_frame, i, s->x, s->y, s->xs, s->ys, s->cols, s->rows, s->code, s->color, s->alpha, s->fade, s->z, s->cz); } }
    if (s23_game.model_fov_max && s23_c435_fov_max > 0.f)   /* models drawn with the frame's narrowest view (Gunmen Wars: include/s23_game.h) */
        for (int i = 0; i < snap->list.count; i++) if (snap->list.e[i].type == S23_RE_MODEL) {
            s23_render_entry *e = &snap->list.e[i]; e->vp_fov = s23_c435_fov_max; e->vp_size_x = s23_c435_fov_vx; e->vp_size_y = s23_c435_fov_vy; }
    { static int last = -1; if (last < 0) { S23_SS_ENV(last); last = getenv("S23_FOVLAST") != NULL; }   /* TEST: models take the clip set current at the list's end */
      if (last) { float s23_c435_fov_now(void); const float f = s23_c435_fov_now(); for (int i = 0; i < snap->list.count; i++) if (snap->list.e[i].type == S23_RE_MODEL) snap->list.e[i].vp_fov = f; } }
    { /* THE WIDESCREEN HUD at the screen edges (Display page "Widescreen HUD", on by default; the user: "make the HUD reach the corners"):
       * a HUD piece is a model placed IN THE SCREEN PLANE -- its depth equals the projection's scale, so one unit is one pixel (Gunmen Wars:
       * the diamond counters at x -+232, the team lists at -+254 / -+280, depth 552-554 against 554.25). In a widescreen picture the left
       * pieces move left and the right ones right by the extra width each side; the world (other depths) stays */
      extern int g_eng_hud_edges_on; int s23_gl_wide(void); const int ex = s23_gl_wide();
      for (int i = 0; i < snap->list.count; i++) snap->list.e[i].clip43 = 0;
      /* THE BARS FADE (the user: "less jarring"): going to 4:3 the wide picture stays and black comes up over the side strips over ~1/3 s,
       * then the 4:3 cut; leaving it, the wide picture is back at once under bars that fade out (s23_gl.c draws them, list.pillar_fade) */
      static int fade;                                              /* 0..20 */
      const int pillar = ex > 0 && s23_game.hud_wide_ok && !s23_game.hud_wide_ok(&snap->list) && s23_game.wide_pillar && s23_game.wide_pillar();
      fade = pillar ? (fade < 20 ? fade + 1 : 20) : (fade > 0 ? fade - 1 : 0);
      snap->list.pillar_fade = (uint8_t)(fade * 255 / 20);
      if (ex > 0 && s23_game.hud_wide_ok && !s23_game.hud_wide_ok(&snap->list)) {   /* not the in-game HUD (select screens ...): their flat
           * pieces stay inside the 4:3 picture -- pieces sliding off its edges (the unpicked level buttons) would show in the side margins */
          for (int i = 0; i < snap->list.count; i++) {
              s23_render_entry *e = &snap->list.e[i];
              if (e->type == S23_RE_IMMEDIATE) e->clip43 = 1;   /* the flat quads (the select screen's buttons and banners) */
              if (s23_game.wide_pillar && s23_game.wide_pillar() && fade >= 20) e->clip43 = 1;   /* the game's option: everything in 4:3 outside a match */
              if (e->type == S23_RE_MODEL && e->vp_fov > 0.f && (float)e->v[2] >= e->vp_fov - 8.f && (float)e->v[2] <= e->vp_fov + 8.f) e->clip43 = 1;
          } }
      else if (g_eng_hud_edges_on && ex > 0 && !s23_game.no_hud_shift)
          for (int i = 0; i < snap->list.count; i++) {
              s23_render_entry *e = &snap->list.e[i];
              if (e->type != S23_RE_MODEL || e->vp_fov <= 0.f || e->v[0] == 0) continue;
              const float z = (float)e->v[2];
              if (z < e->vp_fov - 8.f || z > e->vp_fov + 8.f) continue;
              /* only the pieces out towards the sides (more than 160 of the 320 pixels from the centre): one near the middle (Gunmen Wars'
               * diamond pop-up over the health bar, models 0x0240-3 at x 0..-60) stays whole -- shifting it split its parts apart */
              if (fabsf((float)e->v[0] * e->vp_fov / z) < 160.f) continue;
              const int32_t d = (int32_t)((float)ex * z / e->vp_fov + 0.5f);
              e->v[0] += e->v[0] < 0 ? -d : d;
          } }
    if (s23_game.list_hook) s23_game.list_hook(&snap->list);   /* a game's own fix-ups of the list (Gunmen Wars: the preview windows) */
    s23_render_entries += (unsigned long)l->count;
    { static int lg = -1; if (lg < 0) lg = getenv("S23_RLLOG") != NULL;   /* $S23_RLLOG: the render list drawn each frame (entries) */
      if (lg) fprintf(stderr, "[RL] frame %lu entries %d\n", s23_render_frames, l->count); }
    s23_render_frames++;
    s23_render_cur = !s23_render_cur;
    s23_render[s23_render_cur].count = 0;
    memset(s23_render[s23_render_cur].seg_zoff, 0, sizeof s23_render[s23_render_cur].seg_zoff);
    s23_c435_seg = 0; s23_c435_fov_max = 0.f; s23_c435_fov_plain = 0;
    pthread_mutex_lock(&mu); busy = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
}
/* the newest built frame -- the GPU renderer (main thread). The worker only writes the OTHER slot, and the main thread is the only one that
 * starts a build, so the front slot is stable until the next s23_render_frame_end */
const s23_built *s23_built_latest(void) { pthread_mutex_lock(&mu); const s23_built *b = have_frame ? B[front] : NULL; pthread_mutex_unlock(&mu); return b; }
void s23_render_wait_idle(void) { if (started) wait_idle(); }   /* save states (src/s23_savestate.c) */
const s23_built *s23_built_latest_wait(void) { if (started) wait_idle(); return s23_built_latest(); }
/* motion smoothing: the in-between picture that goes before s23_built_latest() (NULL: none -- smoothing off, the first frame) */
const s23_built *s23_built_mid(int k) { pthread_mutex_lock(&mu); const s23_built *b = have_frame && have_mid && k < mid_n[front] ? MIDS[front][k] : NULL; pthread_mutex_unlock(&mu); return b; }
int s23_built_mid_count(void) { pthread_mutex_lock(&mu); const int n = have_frame && have_mid ? mid_n[front] : 0; pthread_mutex_unlock(&mu); return n; }
void s23_smooth_rate(int pictures_per_s) { smooth_n = pictures_per_s >= 150 ? 2 : 1; }
/* the newest finished software picture (0x00RRGGBB, 640 x 480) */
void s23_frame_rgba(uint32_t *rgba)
{
    pthread_mutex_lock(&mu);
    if (have_frame && soft_raster) memcpy(rgba, frames[front], sizeof frames[0]);
    else if (have_frame) { pthread_mutex_unlock(&mu); s23_gl_read(rgba); return; }
    else memset(rgba, 0, sizeof frames[0]);
    pthread_mutex_unlock(&mu);
}
/* the picture of the frame that just ended -- screenshots: waits for it */
void s23_frame_rgba_wait(uint32_t *rgba) { if (started) wait_idle(); s23_frame_rgba(rgba); }

/* Neo Geo CD video: LSPC registers, palette, sprite and fix rendering.
   Register behaviour and pixel rules after neocd_libretro video.cpp and
   memory_video.cpp; the frame is drawn in one pass (sprite-major), which
   matches the hardware when a game does no mid-frame raster changes.
   Raster effects (sprite tables rewritten line by line during the picture,
   e.g. Riding Hero's road) are logged and drawn per run of lines, see
   "raster" below. */
#include <string.h>
#include "ngcd.h"
#include "video_zoom.h"

static const uint8_t X_ZOOM[256] = {
    0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0, 0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0, 0,0,1,0,1,0,0,0,1,0,0,0,1,0,0,0,
    0,0,1,0,1,0,0,0,1,0,0,0,1,0,1,0, 0,0,1,0,1,0,1,0,1,0,0,0,1,0,1,0,
    0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0, 1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,
    1,0,1,0,1,0,1,0,1,1,1,0,1,0,1,0, 1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,0,
    1,0,1,1,1,0,1,0,1,1,1,0,1,0,1,1, 1,0,1,1,1,0,1,1,1,1,1,0,1,0,1,1,
    1,0,1,1,1,0,1,1,1,1,1,0,1,1,1,1, 1,1,1,1,1,0,1,1,1,1,1,0,1,1,1,1,
    1,1,1,1,1,0,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

/* ------------------------------------------------------------ palette */
static uint16_t conv(uint16_t c) {
#ifdef NGCD_PAL_GS
    /* PS2 GS 16-bit (CT16): A1 B5 G5 R5, red in the low bits; the dark bit
       is dropped (half an LSB) */
    uint16_t gs = (uint16_t)(((c & 0x0F00) >> 7) | ((c & 0x4000) >> 14) |
                             ((c & 0x00F0) << 2) | ((c & 0x2000) >> 8) |
                             ((c & 0x000F) << 11) | ((c & 0x1000) >> 2) | 0x8000);
    if (ng.shadow) gs = (uint16_t)(((gs >> 1) & 0x3DEF) | 0x8000);
    return gs;
#endif
    uint16_t out = (uint16_t)(((c & 0x0F00) << 4) | ((c & 0x4000) >> 3) |
                              ((c & 0x00F0) << 3) | ((c & 0x2000) >> 7) |
                              ((c & 0x000F) << 1) | ((c & 0x1000) >> 12));
    if (!(c & 0x8000)) out |= 0x0020;
    if (ng.shadow) out = (out >> 1) & 0x7BEF;
    return out;
}

uint32_t ngcd_vid_gen = 1;
uint32_t ngcd_tile_gen[NGCD_SPR_SIZE / 128];
uint32_t ngcd_pal_gen[512];
uint32_t ngcd_fix_gen;
uint32_t ngcd_fixdata_gen;

void ngcd_pal_write(uint32_t idx, uint16_t v) {
    if (ng.palram[idx] == v) return;
    ng.palram[idx] = v;
    ng.pal565[idx] = conv(v);
    ngcd_pal_gen[idx >> 4] = ngcd_vid_gen;
    if ((idx & 0xFFF) < 16 * 16) ngcd_fix_gen = ngcd_vid_gen;
}

void ngcd_pal_convert_all(void) {
    for (int i = 0; i < 2 * 4096; i++) ng.pal565[i] = conv(ng.palram[i]);
    for (int i = 0; i < 512; i++) ngcd_pal_gen[i] = ngcd_vid_gen;
    ngcd_fix_gen = ngcd_vid_gen;
}

void ngcd_fix_usage_update(void) {
    ngcd_fix_gen = ngcd_fixdata_gen = ngcd_vid_gen;
    for (int c = 0; c < NGCD_FIX_SIZE / 32; c++) {
        uint8_t any = 0;
        for (int i = 0; i < 32; i++) any |= ng.fix[c * 32 + i];
        ng.fix_usage[c] = any != 0;
    }
}

/* ------------------------------------------------------------ late writes */
#define SCB_BASE  0x8000
#define SCB_WORDS 0x600                       /* SCB2, SCB3, SCB4 */
static int late_valid, late_swapped;
static uint16_t late_scb[SCB_WORDS], live_scb[SCB_WORDS];

/* raster log: sprite table words changed while the picture is drawn
   (row = picture line being drawn; the change shows from the next line) */
#define RLOG_MAX 4096
typedef struct { uint16_t row, off, old, val; } rlog_t;
static rlog_t rlog[RLOG_MAX];
static int rlog_n, rlog_over, rlog_early;
uint32_t ngcd_raster_frames;                  /* statistics: frames drawn in runs of lines */

extern uint32_t ngcd_hbl_irqs;
void ngcd_late_reset(void) { late_valid = 0; rlog_n = 0; rlog_over = 0; rlog_early = 0; ngcd_hbl_irqs = 0; }

/* a raster frame: the game takes raster interrupts during the picture and
   changes the tables then, starting above the rows where Neo Turf
   Masters-like writes for the next frame begin. Games that only run late
   into the picture (no raster interrupts) keep the end-of-frame tables. */
static int raster_on(void) { return rlog_n && rlog_early && !rlog_over && ngcd_hbl_irqs >= 2; }
/* statistics for the PC build: distinct rows / words / late words in this frame's log */
void ngcd_raster_stats(int *rows, int *words, int *late) {
    int r = 0, l = 0, last = -1;
    for (int i = 0; i < rlog_n; i++) {
        if (rlog[i].row != last) { r++; last = rlog[i].row; }
        if (rlog[i].row >= NGCD_FB_H - NGCD_LATE_ROWS) l++;
    }
    *rows = r; *words = rlog_n; *late = l;
}

#ifndef _arch_dreamcast
#include <stdio.h>
void ngcd_rlog_dump(int f) {
    for (int i = 0; i < rlog_n; i++)
        fprintf(stderr, "rl %d row %3d off %03X %04X->%04X\n", f, rlog[i].row, rlog[i].off, rlog[i].old, rlog[i].val);
}
#endif
/* a sprite table word is about to change */
static void late_note(void) {
    int row = ngcd_screen_y() - NGCD_ACTIVE_TOP;
    if (row < NGCD_FB_H - NGCD_LATE_ROWS || row >= NGCD_FB_H) return;
    memcpy(late_scb, &ng.vram[SCB_BASE], sizeof(late_scb));
    late_valid = 1;
}

void ngcd_frame_tables_enter(void) {
    if (!late_valid || late_swapped || raster_on()) return;
    memcpy(live_scb, &ng.vram[SCB_BASE], sizeof(live_scb));
    memcpy(&ng.vram[SCB_BASE], late_scb, sizeof(late_scb));
    late_swapped = 1;
}

void ngcd_frame_tables_leave(void) {
    if (!late_swapped) return;
    memcpy(&ng.vram[SCB_BASE], live_scb, sizeof(live_scb));
    late_swapped = 0;
}

/* ------------------------------------------------------------ LSPC */
uint16_t ngcd_lspc_read(uint32_t a) {
    switch (a) {
    case 0x0: case 0x2: return (uint16_t)ng.vram_data;
    case 0x4: return (uint16_t)ng.vram_modulo;
    case 0x6: {
        uint32_t v = (uint32_t)ngcd_screen_y() + 0x100;
        if (v >= 0x200) v -= NGCD_SCREEN_H_PX;
        return (uint16_t)((v << 7) | (ng.aa_counter & 7));
    }
    }
    return 0xFFFF;
}

void ngcd_lspc_write(uint32_t a, uint16_t v) {
#ifndef _arch_dreamcast
    { extern int ngcd_wtrace; extern int ngcd_screen_x(void);
      if (ngcd_wtrace && a >= 6) fprintf(stderr, "wt %d %d reg%X %04X\n", ngcd_screen_y(), ngcd_screen_x(), a, v); }
#endif
    switch (a) {
    case 0x0:
        ng.vram_offset = v;
        ng.vram_data = ng.vram[ng.vram_offset];
        break;
    case 0x2:
#ifndef _arch_dreamcast
        { extern int ngcd_wtrace; extern int ngcd_screen_x(void);
          if (ngcd_wtrace && (ng.vram_offset == 0x8201 || ng.vram_offset == 0x8401))
              fprintf(stderr, "wt %d %d %04X %04X\n", ngcd_screen_y(), ngcd_screen_x(), ng.vram_offset, v); }
#endif
        if (ng.vram_offset < 0x8800) {
            uint32_t so = ng.vram_offset - SCB_BASE;
            if (so < SCB_WORDS && ng.vram[ng.vram_offset] != v) {
                int row = ngcd_screen_y() - NGCD_ACTIVE_TOP;
                if (row >= 0 && row < NGCD_FB_H) {
                    if (rlog_n < RLOG_MAX) {
                        rlog_t *e = &rlog[rlog_n++];
                        e->row = (uint16_t)row; e->off = (uint16_t)so;
                        e->old = ng.vram[ng.vram_offset]; e->val = v;
                    } else rlog_over = 1;
                    if (row < NGCD_FB_H - NGCD_LATE_ROWS) rlog_early = 1;
                }
                if (!late_valid) late_note();
            }
            ng.vram[ng.vram_offset] = v;
            if ((ng.vram_offset & 0xF000) == 0x7000) ngcd_fix_gen = ngcd_vid_gen;
        }
        ng.vram_offset = (ng.vram_offset & 0x8000) | ((ng.vram_offset + ng.vram_modulo) & 0x7FFF);
        ng.vram_data = ng.vram[ng.vram_offset];
        break;
    case 0x4: ng.vram_modulo = v; break;
    case 0x6:
        ng.aa_speed = v >> 8;
        ng.aa_disabled = (v & 0x0008) != 0;
        ng.hirq_control = v & 0x00F0;
        break;
    case 0x8: ng.hirq_register = (ng.hirq_register & 0x0000FFFF) | ((uint32_t)v << 16); break;
    case 0xA:
        ng.hirq_register = (ng.hirq_register & 0xFFFF0000u) | v;
        if ((ng.hirq_control & 0x20) && ng.hirq_register != 0xFFFFFFFF) {
            uint32_t px = ng.hirq_register + 1;
            if (px > 0x1000000) px = 0x1000000;
            ngcd_timer_arm(T_HBL, ngcd_slice_elapsed() + PIX2M((int32_t)px));
        }
        break;
    case 0xC:
        if (v & 0x02) ng.pending_irq &= ~IRQ_RASTER;
        if (v & 0x04) ng.pending_irq &= ~IRQ_VBL;
        ngcd_update_irq();
        break;
    }
}

void ngcd_vbl_video(void) {
    if (!ng.aa_frame_counter) {
        ng.aa_frame_counter = ng.aa_speed;
        ng.aa_counter++;
    } else {
        ng.aa_frame_counter--;
    }
}

void ngcd_video_reset(void) {
    ng.pal_bank = 0;
    ng.aa_counter = ng.aa_speed = ng.aa_frame_counter = 0;
    ng.aa_disabled = 0;
    ng.spr_disable = ng.fix_disable = 1;
    ng.video_enable = 0;
    ng.shadow = 0;
    ng.hirq_control = 0;
    ng.hirq_register = 0;
    ng.vram_offset = ng.vram_modulo = ng.vram_data = 0;
    memset(ng.fix_usage, 0, sizeof(ng.fix_usage));
    ngcd_pal_convert_all();
}

/* ------------------------------------------------------------ raster */
/* The tables as they were for each run of lines: from the start state (the
   end state with the logged writes undone) the writes are applied again in
   order; run [a, b) ends where the next write shows. */
static uint16_t raster_end_scb[SCB_WORDS];
static int rr_i, rr_a;
static void raster_begin(void) {
    uint16_t *scb = &ng.vram[SCB_BASE];
    memcpy(raster_end_scb, scb, sizeof(raster_end_scb));
    for (int i = rlog_n - 1; i >= 0; i--) scb[rlog[i].off] = rlog[i].old;
    rr_i = 0; rr_a = 0;
}
/* next run of lines with constant tables: 0 when done */
static int raster_next(int *a, int *b) {
    uint16_t *scb = &ng.vram[SCB_BASE];
    if (rr_a >= NGCD_FB_H) return 0;
    while (rr_i < rlog_n && rlog[rr_i].row + 1 <= rr_a) { scb[rlog[rr_i].off] = rlog[rr_i].val; rr_i++; }
    int e = rr_i < rlog_n ? rlog[rr_i].row + 1 : NGCD_FB_H;
    if (e > NGCD_FB_H) e = NGCD_FB_H;
    *a = rr_a; *b = e;
    rr_a = e;
    return 1;
}
static void raster_finish(void) {
    memcpy(&ng.vram[SCB_BASE], raster_end_scb, sizeof(raster_end_scb));
}

/* ------------------------------------------------------------ sprites */
/* one 16-pixel sprite row (pl: pixels 0-7, pr: 8-15, a nibble each) */
/* sprites are drawn into a line buffer with 16 pixels of margin on both
   sides: every visible sprite (x from -15 to 319) fits without clipping */
#define LB_LO (-16)
#define LB_HI (NGCD_FB_W + 16)
static inline __attribute__((always_inline)) void put_row(uint16_t *line, uint32_t x, uint32_t zx,
                        uint32_t pl, uint32_t pr, const uint16_t *pal, uint32_t hflip) {
    int px = (int)x;
    if (x > 0x1F0) px -= 0x200;
    if (zx == 15 && px >= LB_LO && px <= LB_HI - 16) {
        /* fast path: unzoomed and fully on screen */
        uint16_t *o = line + px;
        uint32_t q;
#define PX(k, w, s) q = ((w) >> (s)) & 15; if (q) o[k] = pal[q];
        if (!hflip) {
            PX(0, pl, 0)  PX(1, pl, 4)  PX(2, pl, 8)   PX(3, pl, 12)
            PX(4, pl, 16) PX(5, pl, 20) PX(6, pl, 24)  PX(7, pl, 28)
            PX(8, pr, 0)  PX(9, pr, 4)  PX(10, pr, 8)  PX(11, pr, 12)
            PX(12, pr, 16) PX(13, pr, 20) PX(14, pr, 24) PX(15, pr, 28)
        } else {
            PX(15, pl, 0) PX(14, pl, 4) PX(13, pl, 8)  PX(12, pl, 12)
            PX(11, pl, 16) PX(10, pl, 20) PX(9, pl, 24) PX(8, pl, 28)
            PX(7, pr, 0)  PX(6, pr, 4)  PX(5, pr, 8)   PX(4, pr, 12)
            PX(3, pr, 16) PX(2, pr, 20) PX(1, pr, 24)  PX(0, pr, 28)
        }
#undef PX
        return;
    }
    /* shrunk sprite: the zx+1 source pixels kept by the zoom table */
    static uint8_t zsrc[16][16];
    static int zinit;
    if (!zinit) {
        for (int z = 0; z < 16; z++) {
            int k = 0;
            for (int i = 0; i < 16; i++) if (z == 15 || X_ZOOM[z * 16 + i]) zsrc[z][k++] = (uint8_t)i;
        }
        zinit = 1;
    }
    const uint8_t *src = zsrc[zx];
    int w = (int)zx + 1;
    if (px >= LB_LO && px + w <= LB_HI) {     /* always the case in the line buffer */
        uint16_t *o = line + px;
        uint32_t q;
        if (hflip) { ZROW_FLIP } else { ZROW_NORM }
        return;
    }
    if (hflip) {                              /* h flip: pixel i lands at px + zx - k */
        for (int k = 0; k < w; k++) {
            uint32_t i = src[k];
            uint32_t q = ((i < 8 ? pl >> (i * 4) : pr >> ((i - 8) * 4))) & 15;
            int p = px + (int)zx - k;
            if (q && p >= LB_LO && p < LB_HI) line[p] = pal[q];
        }
        return;
    }
    for (int k = 0; k < w; k++) {
        uint32_t i = src[k];
        uint32_t q = ((i < 8 ? pl >> (i * 4) : pr >> ((i - 8) * 4))) & 15;
        int p = px + k;
        if (q && p >= LB_LO && p < LB_HI) line[p] = pal[q];
    }
}


static int yzoom_identity = -1;            /* zoom row 0xFF maps line -> line */

/* The frame is drawn in bands of 8 lines: inside a band sprites are drawn
   one after another (consecutive rows of a tile share cache lines) into a
   small buffer that stays in the cache, which is then copied out once.
   Drawing straight into the frame touched a new cache line for every
   16-pixel sprite row, which on the SH4 cost as much as the drawing.
   The LSPC shows at most 96 sprites per line: the first 96 (in sprite
   order) covering a line, so sprite n is on line l if it covers l and
   n <= cutoff[l]. */
#define BAND 8
#define NBANDS (NGCD_FB_H / BAND)
static struct { uint16_t y, x, h; uint8_t zx, zy, clip, fast; uint16_t wrap_p, wrap_inv; } sat[382];
static uint16_t cutoff[NGCD_FB_H];
static uint16_t band_list[NBANDS][382];
static uint16_t band_n[NBANDS];
static uint16_t vis_list[382];            /* visible sprites in order (quad builder) */
static int vis_n;

/* rows of the picture (0..223) a sprite covers: up to two intervals */
static int sprite_rows(uint32_t y, uint32_t clip, uint32_t *r0, uint32_t *r1) {
    if (clip >= 0x20) { r0[0] = 0; r1[0] = NGCD_FB_H; return 1; }
    uint32_t h = clip * 0x10, n = 0;
    uint32_t a = y & 0x1FF, e = a + h;                /* scanlines [a, e) mod 512 */
    uint32_t seg[2][2] = { { a, e > 512 ? 512 : e }, { 0, e > 512 ? e - 512 : 0 } };
    for (int i = 0; i < 2; i++) {
        uint32_t s0 = seg[i][0] < NGCD_ACTIVE_TOP ? NGCD_ACTIVE_TOP : seg[i][0];
        uint32_t s1 = seg[i][1] > NGCD_ACTIVE_BOTTOM ? NGCD_ACTIVE_BOTTOM : seg[i][1];
        if (s0 < s1) { r0[n] = s0 - NGCD_ACTIVE_TOP; r1[n] = s1 - NGCD_ACTIVE_TOP; n++; }
    }
    return (int)n;
}

static int cutoff_any;                    /* some line reached 96 sprites */

static void build_sprite_lists(int bands) {
    if (yzoom_identity < 0) {
        yzoom_identity = 1;
        for (int i = 0; i < 256; i++) if (ng.yzoom[0xFF * 256 + i] != i) yzoom_identity = 0;
    }
    static uint8_t count[NGCD_FB_H];
    static int16_t diff[NGCD_FB_H + 1];
    memset(diff, 0, sizeof(diff));
    if (bands) memset(band_n, 0, sizeof(band_n));
    vis_n = 0;
    cutoff_any = 0;
    for (int r = 0; r < NGCD_FB_H; r++) cutoff[r] = 381;
    const uint16_t *scb2 = &ng.vram[0x8000], *scb3 = &ng.vram[0x8200], *scb4 = &ng.vram[0x8400];
    uint32_t x = 0, y = 0, zx = 0xF, zy = 0xFF, clip = 0;
    int maxc = 0;                             /* upper bound of the line counts */
    int exact = 0;                            /* count[] materialised */
    for (uint32_t n = 1; n <= 381; n++) {
        uint32_t a1 = scb2[n], a2 = scb3[n];
        if (a2 & 0x40) {
            x = (x + zx + 1) & 0x1FF;
            zx = (a1 >> 8) & 0xF;
        } else {
            zy = a1 & 0xFF;
            zx = (a1 >> 8) & 0xF;
            clip = a2 & 0x3F;
            y = 0x200 - (a2 >> 7);
            x = scb4[n] >> 7;
        }
        if (!clip) continue;
        uint32_t x1 = x & 0x1FF, x2 = (x + zx + 1) & 0x1FF;
        int visible = (x1 <= 319) || (x2 <= 319);
        /* the lines the sprite covers: clip*16 lines from y (wrapping at
           512), or every line for the 0x20+ (full height) settings */
        uint32_t first, nlines;
        if (clip < 0x20) { first = y; nlines = clip * 0x10; }
        else             { first = NGCD_ACTIVE_TOP; nlines = NGCD_ACTIVE_BOTTOM - NGCD_ACTIVE_TOP; }
        sat[n].x = (uint16_t)x; sat[n].y = (uint16_t)y; sat[n].h = (uint16_t)nlines;
        sat[n].zx = (uint8_t)zx; sat[n].zy = (uint8_t)zy; sat[n].clip = (uint8_t)clip;
        sat[n].fast = zy == 0xFF && yzoom_identity;   /* 0x21 wrap is a no-op at full size */
        if (clip == 0x21) {                   /* zl % p by multiplication (no divide on SH4) */
            sat[n].wrap_p = (uint16_t)((zy + 1) << 1);
            sat[n].wrap_inv = (uint16_t)(0x10000 / sat[n].wrap_p + 1);
        }
        if (visible) vis_list[vis_n++] = (uint16_t)n;
        if (!exact && maxc < 95) {
            /* no line can reach the 96 limit yet: count by intervals */
            maxc++;
            uint32_t r0[2], r1[2];
            int ni = sprite_rows(y, clip, r0, r1);
            for (int i = 0; i < ni; i++) {
                diff[r0[i]]++; diff[r1[i]]--;
                if (bands && visible)
                    for (uint32_t b = r0[i] / BAND; b <= (r1[i] - 1) / BAND; b++)
                        if (!band_n[b] || band_list[b][band_n[b] - 1] != n)
                            band_list[b][band_n[b]++] = (uint16_t)n;
            }
            continue;
        }
        if (!exact) {                         /* from here on, line by line */
            int c = 0;
            for (int r = 0; r < NGCD_FB_H; r++) { c += diff[r]; count[r] = (uint8_t)c; }
            exact = 1;
        }
        int last_band = -1;
        for (uint32_t k = 0; k < nlines; k++) {
            uint32_t sl = (first + k) & 0x1FF;
            if (sl - NGCD_ACTIVE_TOP >= NGCD_ACTIVE_BOTTOM - NGCD_ACTIVE_TOP) continue;
            uint32_t row = sl - NGCD_ACTIVE_TOP;
            if (count[row] >= 96) continue;
            if (++count[row] == 96) { cutoff[row] = (uint16_t)n; cutoff_any = 1; }
            if (bands && visible && (int)(row / BAND) != last_band) {
                last_band = (int)(row / BAND);
                if (!band_n[last_band] || band_list[last_band][band_n[last_band] - 1] != n)
                    band_list[last_band][band_n[last_band]++] = (uint16_t)n;
            }
        }
    }
}

static inline void draw_fix_row(uint16_t *out, int r) {
    const uint16_t *map = &ng.vram[0x7002 + (r >> 3)];
    const uint16_t *palbank = &ng.pal565[ng.pal_bank * 4096];
    for (int col = 0; col < 40; col++, out += 8) {
        uint16_t e = map[col * 32];
        uint32_t ch = e & 0x0FFF;
        if (!ng.fix_usage[ch]) continue;
        const uint8_t *f = &ng.fix[ch * 32 + (r & 7)];
        const uint16_t *pal = &palbank[(e >> 12) * 16];
        uint8_t b;
        b = f[16]; if (b & 0x0F) out[0] = pal[b & 0x0F]; if (b >> 4) out[1] = pal[b >> 4];
        b = f[24]; if (b & 0x0F) out[2] = pal[b & 0x0F]; if (b >> 4) out[3] = pal[b >> 4];
        b = f[0];  if (b & 0x0F) out[4] = pal[b & 0x0F]; if (b >> 4) out[5] = pal[b >> 4];
        b = f[8];  if (b & 0x0F) out[6] = pal[b & 0x0F]; if (b >> 4) out[7] = pal[b >> 4];
    }
}

/* rows [lo, hi) of the picture */
static void render_frame(uint16_t *dst, int pitch, int lo, int hi) {
    if (!ng.video_enable) {
        for (int r = lo; r < hi; r++) memset(dst + r * pitch, 0, NGCD_FB_W * 2);
        return;
    }
    #define LB_W (LB_HI - LB_LO)
    static uint16_t bbuf[BAND][LB_W] __attribute__((aligned(32)));
    uint32_t bg = ng.pal565[ng.pal_bank * 4096 + 4095];
    bg |= bg << 16;
    int sprites = !ng.spr_disable, fix = !ng.fix_disable;
    if (sprites) build_sprite_lists(1);
    const uint16_t *palbank = &ng.pal565[ng.pal_bank * 4096];
    uint32_t aa = ng.aa_disabled ? 0 : 1, aac = ng.aa_counter;
    for (int b = lo / BAND; b < NBANDS && b * BAND < hi; b++) {
        int r0 = b * BAND;
        for (int i = 0; i < BAND; i++) {
            uint32_t *p = (uint32_t *)&bbuf[i][-LB_LO];
            for (int j = 0; j < NGCD_FB_W / 2; j += 4) { p[j] = bg; p[j + 1] = bg; p[j + 2] = bg; p[j + 3] = bg; }
        }
        if (sprites) {
            const uint16_t *lst = band_list[b];
            for (int i = 0, m = band_n[b]; i < m; i++) {
                uint32_t n = lst[i];
                /* walk the sprite's lines in this band; line k of the sprite
                   maps to tile tn, row tl (straight for full-size sprites,
                   through the zoom table when shrunk) */
                uint32_t x = sat[n].x, zx = sat[n].zx, h = sat[n].h, clip = sat[n].clip;
                uint32_t zy = sat[n].zy, fast = sat[n].fast;
                const uint8_t *yz = &ng.yzoom[zy * 256];
                int full = clip >= 0x20;
                uint32_t k = ((uint32_t)r0 + NGCD_ACTIVE_TOP - sat[n].y) & 0x1FF;
                const uint16_t *tm = &ng.vram[n * 64];
                uint32_t tile = 0xFFFF, vflip = 0, hflip = 0;
                const uint32_t *g = 0;
                const uint16_t *pal = 0;
                for (int l = 0; l < BAND; l++, k = (k + 1) & 0x1FF) {
                    if ((!full && k >= h) || n > cutoff[r0 + l]) continue;
                    uint32_t tn, tl;
                    if (fast) {
                        tn = k >> 4; tl = k & 15;
                    } else {
                        uint32_t zl = k & 0xFF;
                        int invert = (k & 0x100) != 0;
                        if (invert) zl ^= 0xFF;
                        if (clip == 0x21) {
                            uint32_t p = sat[n].wrap_p;
                            int r = (int)zl - (int)(((zl * sat[n].wrap_inv) >> 16) * p);
                            if (r < 0) r += (int)p;
                            zl = (uint32_t)r;
                            if (zl > zy) { zl = p - 1 - zl; invert = !invert; }
                        }
                        tn = yz[zl];
                        tl = tn & 0xF;
                        tn >>= 4;
                        if (invert) { tl ^= 0x0F; tn ^= 0x1F; }
                    }
                    if (tn != tile) {
                        tile = tn;
                        uint32_t ti = tm[tile * 2], tc = tm[tile * 2 + 1];
                        if (aa) {
                            if (tc & 0x0008) ti = (ti & ~7u) | (aac & 7);
                            else if (tc & 0x0004) ti = (ti & ~3u) | (aac & 3);
                        }
                        g = &ng.spr[(ti & 0x7FFF) * 32];
                        pal = &palbank[(tc >> 8) * 16];
                        vflip = (tc & 2) ? 15 : 0;
                        hflip = tc & 1;
                    }
                    tl ^= vflip;
                    uint32_t pl = g[16 + tl], pr = g[tl];
                    if (pl | pr) put_row(&bbuf[l][-LB_LO], x, zx, pl, pr, pal, hflip);
                }
            }
        }
        for (int l = 0; l < BAND; l++) {
            if (r0 + l < lo || r0 + l >= hi) continue;
            if (fix) draw_fix_row(&bbuf[l][-LB_LO], r0 + l);
            memcpy(dst + (r0 + l) * pitch, &bbuf[l][-LB_LO], NGCD_FB_W * 2);
        }
    }
}

void ngcd_render(uint16_t *dst, int pitch) {
    if (raster_on()) {                        /* line exact: each run with its tables */
        int a, b;
        raster_begin();
        while (raster_next(&a, &b)) render_frame(dst, pitch, a, b);
        raster_finish();
        return;
    }
    ngcd_frame_tables_enter();
    render_frame(dst, pitch, 0, NGCD_FB_H);
    ngcd_frame_tables_leave();
}

/* ------------------------------------------------------------ quads */
/* The picture as textured rectangles for a hardware renderer: every run of
   lines of one sprite that shows consecutive rows of one tile becomes a
   rectangle (a whole tile for unshrunk sprites). Shrunk sprites skip rows;
   the rows a tile shows are kept as one rectangle and the texture is
   stretched over it, which can differ from the line-exact software result
   by a row. The 96 sprites per line limit is honoured. */
/* line k (0..511) of a shrunk sprite -> tile tn, row tl (before v-flip)
   and the end of the k range that forms one rectangle with it; cached per
   (zoom, 0x21 wrap): info[k] = tn | tl << 5 | run_end << 9 */
#define YMAPS 64
typedef struct { int16_t zy, wrap; uint32_t info[512]; } ymap_t;
static ymap_t ymaps[YMAPS];
static uint8_t ymap_valid[YMAPS];
static int ymap_next;

static const ymap_t *get_ymap(uint32_t zy, int wrap) {
    for (int i = 0; i < YMAPS; i++)
        if (ymap_valid[i] && ymaps[i].zy == (int)zy && ymaps[i].wrap == wrap) return &ymaps[i];
    int slot = ymap_next;
    ymap_next = (ymap_next + 1) & (YMAPS - 1);
    ymap_t *m = &ymaps[slot];
    ymap_valid[slot] = 1;
    m->zy = (int16_t)zy; m->wrap = (int16_t)wrap;
    const uint8_t *yz = &ng.yzoom[zy * 256];
    uint32_t p = (zy + 1) << 1, pinv = 0x10000 / p + 1;
    static uint8_t tn_[512], tl_[512];
    for (uint32_t k = 0; k < 512; k++) {
        uint32_t zl = k & 0xFF;
        int invert = (k & 0x100) != 0;
        if (invert) zl ^= 0xFF;
        if (wrap) {
            int r = (int)zl - (int)(((zl * pinv) >> 16) * p);   /* zl % p */
            if (r < 0) r += (int)p;
            zl = (uint32_t)r;
            if (zl > zy) { zl = p - 1 - zl; invert = !invert; }
        }
        uint32_t tn = yz[zl], tl = tn & 0xF;
        tn >>= 4;
        if (invert) { tl ^= 0x0F; tn ^= 0x1F; }
        tn_[k] = (uint8_t)tn; tl_[k] = (uint8_t)tl;
    }
    /* runs: same tile, row advancing by 1 or 2 in one direction */
    uint32_t start = 0;
    int dir = 0;
    for (uint32_t k = 1; k <= 512; k++) {
        int cont = 0;
        if (k < 512 && tn_[k] == tn_[k - 1]) {
            int d = (int)tl_[k] - (int)tl_[k - 1];
            /* rows of one tile in one direction, any spacing: the strong
               shrinks of scene transitions stay at one rectangle per tile */
            if (wrap)     /* mirrored repeats: keep close to the line-exact picture */
                cont = dir ? (d == dir || d == 2 * dir) : (d == 1 || d == 2 || d == -1 || d == -2);
            else
                cont = d != 0 && (dir ? (d > 0) == (dir > 0) : 1);
            if (cont && !dir) dir = d > 0 ? 1 : -1;
        }
        if (!cont) {
            for (uint32_t j = start; j < k; j++) m->info[j] = tn_[j] | (uint32_t)tl_[j] << 5 | k << 9;
            start = k;
            dir = 0;
        }
    }
    return m;
}

/* the rectangles of sprite n (sat[n]) on picture rows [lo, hi) */
static int emit_sprite(uint32_t n, uint32_t lo, uint32_t hi, ngcd_quad_t *out, int nq, int max) {
    uint32_t aa = ng.aa_disabled ? 0 : 1, aac = ng.aa_counter;
    uint32_t x = sat[n].x, y = sat[n].y, zx = sat[n].zx, zy = sat[n].zy, clip = sat[n].clip;
    uint32_t fast = sat[n].fast;
    const uint8_t *yz = &ng.yzoom[zy * 256];
    const uint16_t *tm = &ng.vram[n * 64];
    int px = (int)x;
    if (x > 0x1F0) px -= 0x200;
    int full = clip >= 0x20;
    if (fast && !cutoff_any) {
        /* unshrunk: one rectangle per tile (split at the picture edges) */
        uint32_t r0[2], r1[2];
        int ni = sprite_rows(y, clip, r0, r1);
        for (int iv = 0; iv < ni; iv++) {
            if (r0[iv] < lo) r0[iv] = lo;
            if (r1[iv] > hi) r1[iv] = hi;
            uint32_t r = r0[iv];
            uint32_t k = (r + NGCD_ACTIVE_TOP - y) & 0x1FF;
            while (r < r1[iv]) {
                uint32_t t = k >> 4, tl = k & 15;
                uint32_t cnt = 16 - tl;
                if (cnt > r1[iv] - r) cnt = r1[iv] - r;
                uint32_t ti = tm[(t & 31) * 2], tc = tm[(t & 31) * 2 + 1];
                if (aa) {
                    if (tc & 0x0008) ti = (ti & ~7u) | (aac & 7);
                    else if (tc & 0x0004) ti = (ti & ~3u) | (aac & 3);
                }
                if (nq < max) {
                    ngcd_quad_t *q = &out[nq++];
                    q->x0 = (int16_t)px; q->x1 = (int16_t)(px + (int)zx + 1);
                    q->y0 = (int16_t)r; q->y1 = (int16_t)(r + cnt);
                    q->tile = (uint16_t)(ti & 0x7FFF);
                    q->pal = (uint16_t)(ng.pal_bank * 256 + (tc >> 8));
                    q->hflip = (uint8_t)(tc & 1);
                    if (tc & 2) { q->v0 = (uint8_t)(16 - tl); q->v1 = (uint8_t)(16 - tl - cnt); }
                    else        { q->v0 = (uint8_t)tl; q->v1 = (uint8_t)(tl + cnt); }
                }
                r += cnt;
                k = (k + cnt) & 0x1FF;
            }
        }
        return nq;
    }
    if (!fast && !cutoff_any) {
        /* shrunk: rectangles from the cached runs of this zoom */
        const ymap_t *m = get_ymap(zy, clip == 0x21);
        uint32_t r0[2], r1[2];
        int ni = sprite_rows(y, clip, r0, r1);
        for (int iv = 0; iv < ni; iv++) {
            if (r0[iv] < lo) r0[iv] = lo;
            if (r1[iv] > hi) r1[iv] = hi;
            uint32_t r = r0[iv];
            uint32_t k = (r + NGCD_ACTIVE_TOP - y) & 0x1FF;
            while (r < r1[iv]) {
                uint32_t info = m->info[k];
                uint32_t cnt = (info >> 9) - k;               /* k never wraps inside a run */
                if (cnt > r1[iv] - r) cnt = r1[iv] - r;
                uint32_t tn = info & 31;
                int a = (int)((info >> 5) & 15), b = (int)((m->info[k + cnt - 1] >> 5) & 15);
                uint32_t ti = tm[tn * 2], tc = tm[tn * 2 + 1];
                if (aa) {
                    if (tc & 0x0008) ti = (ti & ~7u) | (aac & 7);
                    else if (tc & 0x0004) ti = (ti & ~3u) | (aac & 3);
                }
                if (tc & 2) { a = 15 - a; b = 15 - b; }
                if (nq < max) {
                    ngcd_quad_t *q = &out[nq++];
                    q->x0 = (int16_t)px; q->x1 = (int16_t)(px + (int)zx + 1);
                    q->y0 = (int16_t)r; q->y1 = (int16_t)(r + cnt);
                    q->tile = (uint16_t)(ti & 0x7FFF);
                    q->pal = (uint16_t)(ng.pal_bank * 256 + (tc >> 8));
                    q->hflip = (uint8_t)(tc & 1);
                    if (b >= a) { q->v0 = (uint8_t)a; q->v1 = (uint8_t)(b + 1); }
                    else        { q->v0 = (uint8_t)(a + 1); q->v1 = (uint8_t)b; }
                }
                r += cnt;
                k = (k + cnt) & 0x1FF;
            }
        }
        return nq;
    }
    uint32_t first = full ? NGCD_ACTIVE_TOP : y, nlines = sat[n].h;
    /* current run */
    int run = 0, r_row0 = 0, r_last = 0, r_tl0 = 0, r_tll = 0, r_dir = 0;
    uint32_t r_tn = 0, r_ti = 0, r_tc = 0;
    /* the rows [lo, hi) in order (hi: the last run is written out) */
    for (uint32_t rr = lo; rr <= hi; rr++) {
        int ok = 0, row = 0;
        uint32_t tn = 0, tl = 0, ti = 0, tc = 0;
        if (rr < hi) {
            uint32_t sl = rr + NGCD_ACTIVE_TOP;
            row = (int)rr;
            if (((sl - first) & 0x1FF) < nlines && n <= cutoff[row]) {
                uint32_t k = (sl - y) & 0x1FF;
                if (fast) { tn = k >> 4; tl = k & 15; }
                else {
                    uint32_t zl = k & 0xFF;
                    int invert = (k & 0x100) != 0;
                    if (invert) zl ^= 0xFF;
                    if (clip == 0x21) {
                        uint32_t p = sat[n].wrap_p;
                        int r = (int)zl - (int)(((zl * sat[n].wrap_inv) >> 16) * p);
                        if (r < 0) r += (int)p;
                        zl = (uint32_t)r;
                        if (zl > zy) { zl = p - 1 - zl; invert = !invert; }
                    }
                    tn = yz[zl]; tl = tn & 0xF; tn >>= 4;
                    if (invert) { tl ^= 0x0F; tn ^= 0x1F; }
                }
                ti = tm[tn * 2]; tc = tm[tn * 2 + 1];
                if (aa) {
                    if (tc & 0x0008) ti = (ti & ~7u) | (aac & 7);
                    else if (tc & 0x0004) ti = (ti & ~3u) | (aac & 3);
                }
                if (tc & 2) tl ^= 15;
                ok = 1;
            }
        }
        if (run) {
            int d = (int)tl - r_tll;
            int cont = ok && row == r_last + 1 && tn == r_tn && ti == r_ti && tc == r_tc &&
                       (clip == 0x21 ? (r_dir ? (d == r_dir || d == 2 * r_dir) : (d == 1 || d == 2 || d == -1 || d == -2))
                                     : (d != 0 && (r_dir ? (d > 0) == (r_dir > 0) : 1)));
            if (cont) {
                if (!r_dir) r_dir = d > 0 ? 1 : -1;
                r_last = row; r_tll = (int)tl;
                continue;
            }
            /* emit */
            if (nq < max) {
                ngcd_quad_t *q = &out[nq++];
                q->x0 = (int16_t)px; q->x1 = (int16_t)(px + (int)zx + 1);
                q->y0 = (int16_t)r_row0; q->y1 = (int16_t)(r_last + 1);
                q->tile = (uint16_t)(r_ti & 0x7FFF);
                q->pal = (uint16_t)(ng.pal_bank * 256 + (r_tc >> 8));
                q->hflip = (uint8_t)(r_tc & 1);
                if (r_dir >= 0) { q->v0 = (uint8_t)r_tl0; q->v1 = (uint8_t)(r_tll + 1); }
                else            { q->v0 = (uint8_t)(r_tl0 + 1); q->v1 = (uint8_t)r_tll; }
            }
            run = 0;
        }
        if (ok) {
            run = 1; r_row0 = r_last = row; r_tl0 = r_tll = (int)tl; r_dir = 0;
            r_tn = tn; r_ti = ti; r_tc = tc;
        }
    }
    return nq;
}

/* sat[] of sprites [first, last] from the tables as they are now (the
   chain is followed back to its leader, the first sprite without the
   sticky bit); the visible ones go to chain_vis */
static uint16_t chain_vis[382];
static int chain_vis_n;
static void chain_attrs(uint32_t first, uint32_t last) {
    const uint16_t *scb2 = &ng.vram[0x8000], *scb3 = &ng.vram[0x8200], *scb4 = &ng.vram[0x8400];
    uint32_t m = first;
    while (m > 1 && (scb3[m] & 0x40)) m--;
    uint32_t x = 0, y = 0, zx = 0xF, zy = 0xFF, clip = 0;
    chain_vis_n = 0;
    for (uint32_t n = m; n <= last; n++) {
        uint32_t a1 = scb2[n], a2 = scb3[n];
        if (a2 & 0x40) {
            x = (x + zx + 1) & 0x1FF;
            zx = (a1 >> 8) & 0xF;
        } else {
            zy = a1 & 0xFF;
            zx = (a1 >> 8) & 0xF;
            clip = a2 & 0x3F;
            y = 0x200 - (a2 >> 7);
            x = scb4[n] >> 7;
        }
        if (n < first || !clip) continue;
        uint32_t x1 = x & 0x1FF, x2 = (x + zx + 1) & 0x1FF;
        sat[n].x = (uint16_t)x; sat[n].y = (uint16_t)y;
        sat[n].h = (uint16_t)(clip < 0x20 ? clip * 0x10 : NGCD_ACTIVE_BOTTOM - NGCD_ACTIVE_TOP);
        sat[n].zx = (uint8_t)zx; sat[n].zy = (uint8_t)zy; sat[n].clip = (uint8_t)clip;
        sat[n].fast = zy == 0xFF && yzoom_identity;
        if (clip == 0x21) {
            sat[n].wrap_p = (uint16_t)((zy + 1) << 1);
            sat[n].wrap_inv = (uint16_t)(0x10000 / sat[n].wrap_p + 1);
        }
        if ((x1 <= 319) || (x2 <= 319)) chain_vis[chain_vis_n++] = (uint16_t)n;
    }
}

/* Raster frames: the sprites whose tables changed during the picture, with
   the sticky chains they belong to, are drawn run of lines by run of lines
   with the tables of that run; the others once, as usual. The 96 sprites
   per line limit comes from the end-of-frame tables. */
#define CHAINS_MAX 32
#define RQ_MAX 6000
static ngcd_quad_t rq[RQ_MAX];
static uint8_t raster_aff[383];

int ngcd_build_quads(ngcd_quad_t *out, int max) {
    int nq = 0;
    if (!ng.video_enable || ng.spr_disable) return 0;
    build_sprite_lists(0);
    if (!raster_on()) {
        for (int vi = 0; vi < vis_n; vi++) nq = emit_sprite(vis_list[vi], 0, NGCD_FB_H, out, nq, max);
        return nq;
    }
    ngcd_raster_frames++;
    /* changed sprites and their chains (sticky bits as at the end of the frame) */
    const uint16_t *scb3 = &ng.vram[0x8200];
    memset(raster_aff, 0, sizeof(raster_aff));
    for (int i = 0; i < rlog_n; i++) {
        uint32_t n = rlog[i].off & 0x1FF;
        if (n < 1 || n > 381 || raster_aff[n]) continue;
        raster_aff[n] = 1;
        for (uint32_t m = n; m > 1 && (scb3[m] & 0x40); m--) raster_aff[m - 1] = 1;
        for (uint32_t m = n + 1; m <= 381 && (scb3[m] & 0x40); m++) raster_aff[m] = 1;
    }
    uint16_t c_first[CHAINS_MAX], c_last[CHAINS_MAX];
    int c_q0[CHAINS_MAX + 1], nc = 0;
    for (uint32_t n = 1; n <= 381; n++) {
        if (!raster_aff[n] || (n > 1 && raster_aff[n - 1])) continue;
        uint32_t e = n;
        while (e < 381 && raster_aff[e + 1]) e++;
        if (nc == CHAINS_MAX) { for (uint32_t k = n; k <= 381; k++) raster_aff[k] = 0; break; }
        c_first[nc] = (uint16_t)n; c_last[nc] = (uint16_t)e; nc++;
    }
    /* the chains' rectangles, run by run */
    int rn = 0;
    for (int c = 0; c < nc; c++) {
        c_q0[c] = rn;
        int a, b;
        raster_begin();
        while (raster_next(&a, &b)) {
            chain_attrs(c_first[c], c_last[c]);
            for (int i = 0; i < chain_vis_n; i++)
                rn = emit_sprite(chain_vis[i], (uint32_t)a, (uint32_t)b, rq, rn, RQ_MAX);
        }
        raster_finish();
    }
    c_q0[nc] = rn;
    /* everything in sprite order: a chain goes where its first sprite is */
    int c = 0;
    for (int vi = 0; vi <= vis_n; vi++) {
        uint32_t n = vi < vis_n ? vis_list[vi] : 382;
        while (c < nc && c_first[c] <= n) {
            for (int i = c_q0[c]; i < c_q0[c + 1] && nq < max; i++) out[nq++] = rq[i];
            c++;
        }
        if (n <= 381 && !raster_aff[n]) nq = emit_sprite(n, 0, NGCD_FB_H, out, nq, max);
    }
    return nq;
}

/* the fix layer of one screen row, palette indexes 1-15 (0 = transparent),
   for renderers that keep their own colours */
void ngcd_fix_row_raw(uint16_t *out, int r, const uint16_t *pal1555) {
    const uint16_t *map = &ng.vram[0x7002 + (r >> 3)];
    for (int col = 0; col < 40; col++, out += 8) {
        uint16_t e = map[col * 32];
        uint32_t ch = e & 0x0FFF;
        if (!ng.fix_usage[ch]) { for (int i = 0; i < 8; i++) out[i] = 0; continue; }
        const uint8_t *f = &ng.fix[ch * 32 + (r & 7)];
        const uint16_t *pal = &pal1555[(e >> 12) * 16];
        static const uint8_t ord[4] = { 16, 24, 0, 8 };
        for (int k = 0; k < 4; k++) {
            uint8_t b = f[ord[k]];
            out[k * 2]     = (b & 0x0F) ? pal[b & 0x0F] : 0;
            out[k * 2 + 1] = (b >> 4)   ? pal[b >> 4]   : 0;
        }
    }
}

/* Neo Geo colour -> ARGB1555 (opaque), with the shadow mode applied */
uint16_t ngcd_color_1555(uint16_t c) {
    uint32_t r = ((c >> 7) & 0x1E) | ((c >> 14) & 1);
    uint32_t g = ((c >> 3) & 0x1E) | ((c >> 13) & 1);
    uint32_t b = ((c << 1) & 0x1E) | ((c >> 12) & 1);
    if (ng.shadow) { r >>= 1; g >>= 1; b >>= 1; }
    return (uint16_t)(0x8000 | (r << 10) | (g << 5) | b);
}

#ifdef NGCD_QUAD_CHECK
/* PC build: the rectangles of ngcd_build_quads() rasterized the way the
   PVR draws them (texel at each pixel centre, colour 0 transparent),
   background and fix layer as the software renderer does; the number of
   pixels that differ from ngcd_render()'s picture */
uint16_t *ngcd_quad_check_pic;
int ngcd_quad_check(const uint16_t *sw, int pitch, int *fx, int *fy) {
    static ngcd_quad_t q[20000];
    static uint16_t pic[NGCD_FB_H][NGCD_FB_W];
    ngcd_quad_check_pic = &pic[0][0];
    ngcd_frame_tables_enter();
    int nq = ngcd_build_quads(q, 20000);
    ngcd_frame_tables_leave();
    uint16_t bg = ng.video_enable ? ng.pal565[ng.pal_bank * 4096 + 4095] : 0;
    for (int y = 0; y < NGCD_FB_H; y++) for (int x = 0; x < NGCD_FB_W; x++) pic[y][x] = bg;
    if (ng.video_enable)
        for (int i = 0; i < nq; i++) {
            const ngcd_quad_t *r = &q[i];
            int w = r->x1 - r->x0, h = r->y1 - r->y0;
            const uint32_t *g = &ng.spr[(uint32_t)r->tile * 32];
            for (int y = r->y0; y < r->y1; y++) {
                if (y < 0 || y >= NGCD_FB_H) continue;
                float fv = r->v0 + (r->v1 - r->v0) * ((y - r->y0) + 0.5f) / h;
                int tl = (int)fv; if (tl > 15) tl = 15; if (tl < 0) tl = 0;
                uint32_t pl = g[16 + tl], pr = g[tl];
                for (int x = r->x0; x < r->x1; x++) {
                    if (x < 0 || x >= NGCD_FB_W) continue;
                    int u = (int)(((x - r->x0) + 0.5f) * 16 / w);
                    if (u > 15) u = 15;
                    if (r->hflip) u = 15 - u;
                    uint32_t c = (u < 8 ? pl >> (u * 4) : pr >> ((u - 8) * 4)) & 15;
                    if (c) pic[y][x] = ng.pal565[(uint32_t)r->pal * 16 + c];
                }
            }
        }
    int bad = 0;
    *fx = *fy = -1;
    for (int y = 0; y < NGCD_FB_H; y++) {
        if (ng.video_enable && !ng.fix_disable) {
            static uint16_t lb[LB_W];
            memcpy(&lb[-LB_LO], pic[y], sizeof(pic[y]));
            draw_fix_row(&lb[-LB_LO], y);
            memcpy(pic[y], &lb[-LB_LO], sizeof(pic[y]));
        }
        for (int x = 0; x < NGCD_FB_W; x++)
            if (pic[y][x] != sw[y * pitch + x]) { if (!bad) { *fx = x; *fy = y; } bad++; }
    }
    return bad;
}
#endif

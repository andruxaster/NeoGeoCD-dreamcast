/* Neo Geo CD picture drawn by the PowerVR (idea after NEO4ALL's GL path):
   sprite tiles are converted once per (tile, palette) into a cache of 4096
   separate 16x16 ARGB1555 textures in video memory (colour 0 = clear) and
   every frame becomes a list of textured rectangles in the punch-through
   list (ngcd_build_quads), drawn in sprite order by increasing depth. The fix
   layer is a 512x256 8bpp twiddled texture using the PVR palette (index =
   palette << 4 | colour, colour 0 transparent): only the 8x8 cells whose map
   entry changed are rewritten, and a palette change only rewrites 256
   palette entries. The CPU only
   builds the rectangle list; filling the pixels is left to the PVR.

   Cache entries are checked against the core's change generations: a tile
   or palette written after an entry was made invalidates it. Slots used by
   the frame being rendered or the one before are never rewritten.

   Every slot is its own small texture (as in NEO4ALL), not a region of one
   big atlas: emulators such as Flycast decode textures on their render
   thread while the SH4 already runs the next frame, and writes landing in a
   texture while it is decoded can be lost. A slot rewritten now is never one
   the previous frame uses, so it is never being decoded. The fix layer
   alternates between two textures for the same reason.

   There is no software picture: if a frame has more rectangles than the
   vertex buffer holds, the frontmost ones are left out; if the tile cache
   is full of tiles in use, the rectangles without a slot are left out
   (both counted in ngcd_pvr_dropped; neither was seen in the tested games). */
#include <kos.h>
#include <string.h>
#include "../core/ngcd.h"

#define SLOT_BYTES  (16 * 16 * 2)
#define SLOTS       4096
#define HASH        4096
#define MAX_QUADS   9000         /* x (128 + 32) bytes: fits the 1.5 MB vertex buffer */

typedef struct {
    uint32_t key;            /* tile | pal << 15, 0xFFFFFFFF = free */
    uint32_t made, used;     /* generations */
    uint16_t next;           /* hash chain, 0xFFFF = end */
    uint8_t  empty, pad;
} slot_t;

static slot_t slots[SLOTS];
static uint16_t bucket[HASH];
static uint32_t hand;
static pvr_ptr_t atlas, fixtex[2];
static int fix_cur;
uint32_t ngcd_pvr_dropped;
static pvr_poly_hdr_t hdr_tile, hdr_fix[2], hdr_bg;
static ngcd_quad_t quads[MAX_QUADS];
static uint16_t quad_slot[MAX_QUADS];
static uint32_t fix_made;
/* per fix texture (they alternate): map entries it was drawn with, the
   generation it was made at, the palette bank it holds */
#define FIX_COLS 40
#define FIX_ROWS (NGCD_FB_H / 8)
static uint16_t fix_shadow[2][FIX_COLS * FIX_ROWS];
static uint32_t fix_tex_made[2];
static int fix_tex_bank[2], fix_tex_valid[2];
static uint32_t fix_cell_base[FIX_COLS * FIX_ROWS];  /* byte offset of each cell */
static uint8_t fix_tw[64];                     /* twiddled offset of (x, y) in a cell */
static int ready;

uint32_t ngcd_pvr_tiles_made, ngcd_pvr_quads, ngcd_pvr_overflow;

static inline uint32_t hash_of(uint32_t key) { return (key ^ (key >> 12) ^ (key >> 20)) & (HASH - 1); }

static void unlink_slot(uint32_t s) {
    uint32_t b = hash_of(slots[s].key);
    uint16_t *p = &bucket[b];
    while (*p != 0xFFFF) {
        if (*p == s) { *p = slots[s].next; break; }
        p = &slots[*p].next;
    }
    slots[s].key = 0xFFFFFFFF;
}

int ngcd_pvr_init(void) {
    atlas = pvr_mem_malloc(SLOTS * SLOT_BYTES);
    for (int i = 0; i < 2; i++) {
        fixtex[i] = pvr_mem_malloc(512 * 256);
        if (!fixtex[i]) return -1;
    }
    if (!atlas) return -1;
    for (int i = 0; i < SLOTS; i++) { slots[i].key = 0xFFFFFFFF; slots[i].used = 0; slots[i].next = 0xFFFF; }
    for (int i = 0; i < HASH; i++) bucket[i] = 0xFFFF;

    pvr_poly_cxt_t cxt;
    /* header of slot 0; slot s differs only in the texture address
       (mode3 bits 0-24 hold address / 8) */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     16, 16, atlas, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr_tile, &cxt);
    for (int i = 0; i < 2; i++) {
        /* 8bpp palette bank i (entries 256 * i ...) */
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(i) | PVR_TXRFMT_TWIDDLED,
                         512, 256, fixtex[i], PVR_FILTER_NONE);
        pvr_poly_compile(&hdr_fix[i], &cxt);
        fix_tex_valid[i] = 0;
    }
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr_bg, &cxt);
    PVR_SET(PVR_PT_ALPHA_REF, 0x80);           /* ARGB1555: alpha 0 or 255 */
    pvr_set_pal_format(PVR_PAL_ARGB1555);
    /* twiddled layout (as pvr_txr_load_ex): texel (x, y) of a 512x256
       texture is at (x / 256) * 65536 + tw(y) | tw(x % 256) << 1, tw()
       spreading the bits to every other position; an aligned 8x8 cell is
       64 consecutive bytes */
    for (int i = 0; i < 64; i++) {
        uint32_t x = i & 7, y = i >> 3, o = 0;
        for (int b = 0; b < 3; b++) o |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
        fix_tw[i] = (uint8_t)o;
    }
    for (int cy = 0; cy < FIX_ROWS; cy++)
        for (int cx = 0; cx < FIX_COLS; cx++) {
            uint32_t x = cx * 8, y = cy * 8, o = 0;
            for (int b = 0; b < 8; b++) o |= ((y >> b) & 1) << (2 * b) | (((x & 255) >> b) & 1) << (2 * b + 1);
            fix_cell_base[cy * FIX_COLS + cx] = (x >> 8) * 65536 + o;
        }
    fix_made = 0;
    ready = 1;
    return 0;
}

/* convert one tile with one palette into atlas slot s */
static void make_tile(uint32_t s, uint32_t tile, uint32_t pal) {
    uint16_t col[16];
    col[0] = 0;
    const uint16_t *pr = &ng.palram[pal * 16];
    for (int c = 1; c < 16; c++) col[c] = ngcd_color_1555(pr[c]);
    const uint32_t *g = &ng.spr[tile * 32];
    volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)atlas + s * SLOT_BYTES);
    uint32_t any = 0;
    for (int r = 0; r < 16; r++) {
        uint32_t a = g[16 + r], b = g[r];          /* pixels 0-7, 8-15 */
        any |= a | b;
        dst[0] = col[a & 15] | (uint32_t)col[(a >> 4) & 15] << 16;
        dst[1] = col[(a >> 8) & 15] | (uint32_t)col[(a >> 12) & 15] << 16;
        dst[2] = col[(a >> 16) & 15] | (uint32_t)col[(a >> 20) & 15] << 16;
        dst[3] = col[(a >> 24) & 15] | (uint32_t)col[a >> 28] << 16;
        dst[4] = col[b & 15] | (uint32_t)col[(b >> 4) & 15] << 16;
        dst[5] = col[(b >> 8) & 15] | (uint32_t)col[(b >> 12) & 15] << 16;
        dst[6] = col[(b >> 16) & 15] | (uint32_t)col[(b >> 20) & 15] << 16;
        dst[7] = col[(b >> 24) & 15] | (uint32_t)col[b >> 28] << 16;
        dst += 8;
    }
    slots[s].empty = any == 0;
    ngcd_pvr_tiles_made++;
}

/* slot holding (tile, pal) for generation gen, made if needed; 0xFFFF if the
   cache is full of tiles still in use */
static uint32_t get_slot(uint32_t tile, uint32_t pal, uint32_t gen) {
    uint32_t key = tile | pal << 15, b = hash_of(key);
    for (uint32_t s = bucket[b]; s != 0xFFFF; s = slots[s].next) {
        if (slots[s].key != key) continue;
        if (slots[s].made > ngcd_tile_gen[tile] && slots[s].made > ngcd_pal_gen[pal]) {
            slots[s].used = gen;
            return s;
        }
        unlink_slot(s);            /* stale: leave the slot to expire, make a new one */
        break;
    }
    for (uint32_t tries = 0; tries < SLOTS; tries++) {
        uint32_t s = hand;
        hand = (hand + 1) & (SLOTS - 1);
        if (slots[s].used + 1 >= gen) continue;   /* drawn this frame or the last */
        if (slots[s].key != 0xFFFFFFFF) unlink_slot(s);
        slots[s].key = key;
        slots[s].made = gen;
        slots[s].used = gen;
        slots[s].next = bucket[b];
        bucket[b] = (uint16_t)s;
        make_tile(s, tile, pal);
        return s;
    }
    ngcd_pvr_overflow++;
    return 0xFFFF;
}

static inline void vtx(uint32_t flags, float x, float y, float z, float u, float v, uint32_t argb) {
    pvr_vertex_t *p = pvr_dr_target();
    p->flags = flags; p->x = x; p->y = y; p->z = z; p->u = u; p->v = v;
    p->argb = argb; p->oargb = 0;
    pvr_dr_commit(p);
}

static inline void header(const pvr_poly_hdr_t *h) {
    pvr_poly_hdr_t *p = pvr_dr_target();
    memcpy(p, h, sizeof(*h));
    pvr_dr_commit(p);
}

/* one 8x8 cell of fix texture t from map entry e */
static void fix_cell(int t, uint32_t k, uint16_t e) {
    uint32_t cell[16];
    uint8_t *c = (uint8_t *)cell;
    uint32_t ch = e & 0x0FFF, pal = (uint32_t)(e >> 12) << 4;
    if (!ng.fix_usage[ch]) {
        memset(cell, 0, sizeof(cell));
    } else {
        static const uint8_t ord[4] = { 16, 24, 0, 8 };
        const uint8_t *f = &ng.fix[ch * 32];
        for (int y = 0; y < 8; y++)
            for (int k2 = 0; k2 < 4; k2++) {
                uint32_t b = f[ord[k2] + y], lo = b & 15, hi = b >> 4;
                c[fix_tw[y * 8 + k2 * 2]]     = (uint8_t)(lo ? pal | lo : 0);
                c[fix_tw[y * 8 + k2 * 2 + 1]] = (uint8_t)(hi ? pal | hi : 0);
            }
    }
    volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)fixtex[t] + fix_cell_base[k]);
    for (int i = 0; i < 16; i++) dst[i] = cell[i];
}

/* bring the fix texture not on screen up to date and show it next */
static void update_fix(uint32_t gen) {
    int t = fix_cur ^ 1;
    int full = !fix_tex_valid[t] || ngcd_fixdata_gen >= fix_tex_made[t];
    uint16_t *sh = fix_shadow[t];
    for (int cx = 0; cx < FIX_COLS; cx++) {
        const uint16_t *map = &ng.vram[0x7002 + cx * 32];   /* rows 2-29 are visible */
        for (int cy = 0; cy < FIX_ROWS; cy++) {
            uint32_t k = cy * FIX_COLS + cx;
            uint16_t e = map[cy];
            if (!full && sh[k] == e) continue;
            sh[k] = e;
            fix_cell(t, k, e);
        }
    }
    /* palettes 0-15 of the current bank -> PVR palette bank t */
    int bank = ng.pal_bank;
    int pal_new = !fix_tex_valid[t] || fix_tex_bank[t] != bank;
    for (int p = 0; p < 16 && !pal_new; p++)
        if (ngcd_pal_gen[bank * 256 + p] >= fix_tex_made[t]) pal_new = 1;
    if (pal_new) {
        const uint16_t *pr = &ng.palram[bank * 4096];
        for (int i = 0; i < 256; i++)
            pvr_set_pal_entry(t * 256 + i, (i & 15) ? ngcd_color_1555(pr[i]) : 0);
    }
    fix_tex_bank[t] = bank;
    fix_tex_made[t] = gen;
    fix_tex_valid[t] = 1;
    fix_cur = t;
    fix_made = gen;
}

/* call between pvr_scene_begin() and pvr_scene_finish(); the picture's
   top left corner goes to (ox, oy) on screen */
void ngcd_pvr_draw(float ox, float oy) {
    if (!ready) return;
    uint32_t gen = ++ngcd_vid_gen;
    ngcd_frame_tables_enter();
    int nq = ngcd_build_quads(quads, MAX_QUADS);
    ngcd_frame_tables_leave();
    ngcd_pvr_quads += (uint32_t)nq;
    if (nq >= MAX_QUADS) ngcd_pvr_dropped++;  /* the list was cut short */
    /* tiles first: the store queues are needed for the vertex stream below */
    for (int i = 0; i < nq; i++) {
        quad_slot[i] = (uint16_t)get_slot(quads[i].tile, quads[i].pal, gen);
        if (quad_slot[i] == 0xFFFF) ngcd_pvr_dropped++;
    }
    int fix = ng.video_enable && !ng.fix_disable;
    if (fix && ngcd_fix_gen >= fix_made) update_fix(gen);

    /* background colour (palette 4095), or black while video is disabled */
    uint32_t bg = 0xFF000000;
    if (ng.video_enable) {
        uint16_t c = ngcd_color_1555(ng.palram[ng.pal_bank * 4096 + 4095]);
        bg = 0xFF000000 | ((c >> 10 & 31) << 19) | ((c >> 5 & 31) << 11) | ((c & 31) << 3);
    }
    pvr_list_begin(PVR_LIST_OP_POLY);
    header(&hdr_bg);
    float x1 = ox + NGCD_FB_W, y1 = oy + NGCD_FB_H;
    vtx(PVR_CMD_VERTEX, ox, oy, 0.5f, 0, 0, bg);
    vtx(PVR_CMD_VERTEX, x1, oy, 0.5f, 0, 0, bg);
    vtx(PVR_CMD_VERTEX, ox, y1, 0.5f, 0, 0, bg);
    vtx(PVR_CMD_VERTEX_EOL, x1, y1, 0.5f, 0, 0, bg);
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_PT_POLY);
    const uint32_t mode3 = hdr_tile.mode3;
    uint32_t last = 0xFFFF;
    float z = 1.0f;
    for (int i = 0; i < nq; i++) {
        uint32_t s = quad_slot[i];
        if (s == 0xFFFF || slots[s].empty) continue;
        const ngcd_quad_t *q = &quads[i];
        if (s != last) {                      /* this slot's texture */
            pvr_poly_hdr_t *h = pvr_dr_target();
            memcpy(h, &hdr_tile, sizeof(*h));
            h->mode3 = mode3 + s * (SLOT_BYTES / 8);
            pvr_dr_commit(h);
            last = s;
        }
        float u0 = q->hflip ? 1.0f : 0.0f, u1 = 1.0f - u0;
        float v0 = q->v0 * (1.0f / 16), v1 = q->v1 * (1.0f / 16);
        float qx0 = ox + q->x0, qx1 = ox + q->x1, qy0 = oy + q->y0, qy1 = oy + q->y1;
        z += 1.0f / 65536.0f;
        vtx(PVR_CMD_VERTEX, qx0, qy0, z, u0, v0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, qx1, qy0, z, u1, v0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, qx0, qy1, z, u0, v1, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX_EOL, qx1, qy1, z, u1, v1, 0xFFFFFFFF);
    }
    if (fix) {
        header(&hdr_fix[fix_cur]);
        float u1 = NGCD_FB_W / 512.0f, v1 = NGCD_FB_H / 256.0f;
        vtx(PVR_CMD_VERTEX, ox, oy, 2.0f, 0, 0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, x1, oy, 2.0f, u1, 0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, ox, y1, 2.0f, 0, v1, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX_EOL, x1, y1, 2.0f, u1, v1, 0xFFFFFFFF);
    }
    pvr_list_finish();
}

/* Neo Geo CD picture drawn by the PowerVR (idea after NEO4ALL's GL path):
   sprite tiles are converted once per (tile, palette) into a cache of 4096
   separate 16x16 ARGB1555 textures in video memory (colour 0 = clear) and
   every frame becomes a list of textured rectangles in the punch-through
   list (ngcd_build_quads), drawn in sprite order by increasing depth. The fix
   layer is a 320x224 texture redrawn only when it changes. The CPU only
   builds the rectangle list; filling the pixels is left to the PVR.

   Cache entries are checked against the core's change generations: a tile
   or palette written after an entry was made invalidates it. Slots used by
   the frame being rendered or the one before are never rewritten.

   Every slot is its own small texture (as in NEO4ALL), not a region of one
   big atlas: emulators such as Flycast decode textures on their render
   thread while the SH4 already runs the next frame, and writes landing in a
   texture while it is decoded can be lost. A slot rewritten now is never one
   the previous frame uses, so it is never being decoded. The fix layer and
   the software fallback picture alternate between two textures for the
   same reason. */
#include <kos.h>
#include <string.h>
#include <malloc.h>
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
static pvr_ptr_t atlas, fixtex[2], swtex[2];
static pvr_poly_hdr_t hdr_sw[2];
static int fix_cur, sw_cur;
static uint16_t *swbuf;                       /* software picture, 512 x 224 */
uint32_t ngcd_pvr_fallbacks;
static pvr_poly_hdr_t hdr_tile, hdr_fix[2], hdr_bg;
static ngcd_quad_t quads[MAX_QUADS];
static uint16_t quad_slot[MAX_QUADS];
static uint16_t fixbuf[512 * NGCD_FB_H] __attribute__((aligned(32)));
static uint32_t fix_made;
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
        fixtex[i] = pvr_mem_malloc(512 * 256 * 2);
        swtex[i] = pvr_mem_malloc(512 * 256 * 2);
        if (!fixtex[i] || !swtex[i]) return -1;
    }
    swbuf = memalign(32, 512 * NGCD_FB_H * 2);
    if (!atlas || !swbuf) return -1;
    for (int i = 0; i < SLOTS; i++) { slots[i].key = 0xFFFFFFFF; slots[i].used = 0; slots[i].next = 0xFFFF; }
    for (int i = 0; i < HASH; i++) bucket[i] = 0xFFFF;

    pvr_poly_cxt_t cxt;
    /* header of slot 0; slot s differs only in the texture address
       (mode3 bits 0-24 hold address / 8) */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     16, 16, atlas, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr_tile, &cxt);
    for (int i = 0; i < 2; i++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                         512, 256, fixtex[i], PVR_FILTER_NONE);
        pvr_poly_compile(&hdr_fix[i], &cxt);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         512, 256, swtex[i], PVR_FILTER_NONE);
        pvr_poly_compile(&hdr_sw[i], &cxt);
    }
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr_bg, &cxt);
    PVR_SET(PVR_PT_ALPHA_REF, 0x80);           /* ARGB1555: alpha 0 or 255 */
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

static void update_fix(uint32_t gen) {
    uint16_t pal[16 * 16];
    const uint16_t *pr = &ng.palram[ng.pal_bank * 4096];
    for (int i = 0; i < 256; i++) pal[i] = ngcd_color_1555(pr[i]);
    for (int r = 0; r < NGCD_FB_H; r++) ngcd_fix_row_raw(&fixbuf[r * 512], r, pal);
    fix_cur ^= 1;                             /* not the one being shown */
    pvr_txr_load(fixbuf, fixtex[fix_cur], sizeof(fixbuf));
    fix_made = gen;
}

/* call between pvr_scene_begin() and pvr_scene_finish(); the picture's
   top left corner goes to (ox, oy) on screen */
void ngcd_pvr_draw(float ox, float oy) {
    if (!ready) return;
    uint32_t gen = ++ngcd_vid_gen;
    int nq = ngcd_build_quads(quads, MAX_QUADS);
    ngcd_pvr_quads += (uint32_t)nq;
    /* tiles first: the store queues are needed for the vertex stream below */
    int ok = nq < MAX_QUADS;
    for (int i = 0; i < nq && ok; i++) {
        quad_slot[i] = (uint16_t)get_slot(quads[i].tile, quads[i].pal, gen);
        if (quad_slot[i] == 0xFFFF) ok = 0;
    }
    if (!ok) {
        /* more rectangles than the vertex buffer holds (extreme shrink
           effects) or a full tile cache: draw this frame in software */
        ngcd_pvr_fallbacks++;
        ngcd_render(swbuf, 512);
        sw_cur ^= 1;
        pvr_txr_load(swbuf, swtex[sw_cur], 512 * NGCD_FB_H * 2);
        pvr_list_begin(PVR_LIST_OP_POLY);
        header(&hdr_sw[sw_cur]);
        float x1 = ox + NGCD_FB_W, y1 = oy + NGCD_FB_H;
        float u1 = NGCD_FB_W / 512.0f, v1 = NGCD_FB_H / 256.0f;
        vtx(PVR_CMD_VERTEX, ox, oy, 1.0f, 0, 0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, x1, oy, 1.0f, u1, 0, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX, ox, y1, 1.0f, 0, v1, 0xFFFFFFFF);
        vtx(PVR_CMD_VERTEX_EOL, x1, y1, 1.0f, u1, v1, 0xFFFFFFFF);
        pvr_list_finish();
        fix_made = 0;                         /* the fix texture was not refreshed */
        return;
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

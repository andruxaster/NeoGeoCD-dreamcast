/* NGCD-DC Dreamcast / KallistiOS frontend, extracted from dc-poker r13.
   The picture lives in an RGB565 texture; the PVR scales it to the screen.
   Only 8-line rows that changed are uploaded each frame. */
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include "machine.h"

#ifdef NGCD_RELEASE
/* INIT_QUIET disables dbgio entirely: no banner, no dbglog output */
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_QUIET);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

#define DATA_DIR    "/cd/"
#define AUDIO_BUF   8192            /* bytes */
#define MAX_FILES   16

static uint16_t *fb;                /* tex_w x height, 32-byte aligned */
static int16_t   abuf[AUDIO_BUF / 2] __attribute__((aligned(32)));
static uint32_t  audio_rate;

static void fatal(const char *msg) {
    dbglog(DBG_ERROR, "%s: %s\n", machine.title, msg);
    vid_clear(0, 0, 64);
    bfont_draw_str(vram_s + 24 * vid_mode->width + 8, vid_mode->width, true, msg);
    for (;;)
        thd_sleep(1000);
}

static uint8_t *load(const mach_file_t *mf, uint32_t *out_size) {
    char path[64], m[96];
    snprintf(path, sizeof(path), DATA_DIR "%s", mf->name);

    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) {
        if (mf->required) {
            snprintf(m, sizeof(m), "Missing %s", path);
            fatal(m);
        }
        dbglog(DBG_WARNING, "%s: optional %s not found\n", machine.title, path);
        *out_size = 0;
        return NULL;
    }
    ssize_t sz = fs_total(f);
    if (mf->size && sz != (ssize_t)mf->size && !(mf->alt_size && sz == (ssize_t)mf->alt_size)) {
        snprintf(m, sizeof(m), "%s: size %d, expected %d", path, (int)sz, (int)mf->size);
        fatal(m);
    }
    uint8_t *buf = malloc(sz);
    if (!buf || fs_read(f, buf, sz) != sz)
        fatal("read error");
    fs_close(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* snd_stream asks for BYTES despite the parameter name (see snd_stream.c) */
static void *audio_cb(snd_stream_hnd_t hnd, int req_bytes, int *got_bytes) {
    (void)hnd;
    int n = req_bytes / 2;
    if (n > AUDIO_BUF / 2)
        n = AUDIO_BUF / 2;
    if (machine.audio_stereo)
        n &= ~1;
    machine.audio(abuf, n, audio_rate);
    *got_bytes = n * 2;
    return abuf;
}

/* interframe blending (machine.interframe_blend): the previous frame is
   drawn at 50% over the current one, so 30 Hz flicker (Neo Geo shadows)
   looks translucent as on a CRT. R trigger toggles it. */
static int blend_on;
static int hw_render;                 /* machine.pvr_draw in use */
static void blend_toggle_check(int rtrig) {
    static int was;
    int down = rtrig > 128;
    if (down && !was) {
        if (machine.pvr_draw) hw_render = !hw_render;     /* R: PVR <-> software */
        else if (machine.interframe_blend) blend_on = !blend_on;
    }
    was = down;
}

static void poll_pad(int *quit) {
    if (machine.set_pad_n) {
        for (int p = 0; p < 4; p++) {
            maple_device_t *d = maple_enum_type(p, MAPLE_FUNC_CONTROLLER);
            cont_state_t *s = d ? (cont_state_t *)maple_dev_status(d) : NULL;
            if (!s) { machine.set_pad_n(p, 0, 0, 0); continue; }
            machine.set_pad_n(p, s->buttons, s->ltrig, s->rtrig);
            if (p == 0) {
                blend_toggle_check(s->rtrig);
                const uint32_t exit_combo = CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y;
                if ((s->buttons & exit_combo) == exit_combo) *quit = 1;
            }
        }
        return;
    }
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    if (!st) {
        machine.set_pad(0, 0, 0);
        return;
    }
    machine.set_pad(st->buttons, st->ltrig, st->rtrig);
    blend_toggle_check(st->rtrig);

    const uint32_t exit_combo = CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y;
    if ((st->buttons & exit_combo) == exit_combo)
        *quit = 1;
}

/* Upload changed rows; consecutive rows go in one SQ transfer. */
static void upload_rows(pvr_ptr_t tex, uint64_t rows) {
    const int nrows = machine.height / 8;
    int r = 0;
    while (r < nrows) {
        if (!(rows & (1ull << r))) { r++; continue; }
        int start = r;
        while (r < nrows && (rows & (1ull << r)))
            r++;
        size_t off   = (size_t)start * 8 * machine.tex_w;
        size_t bytes = (size_t)(r - start) * 8 * machine.tex_w * 2;
        pvr_txr_load(fb + off, (uint16_t *)tex + off, bytes);
    }
}

static void draw_screen(const pvr_poly_hdr_t *hdr, uint32_t argb) {
    float x0, x1, y0, y1;
    if (machine.video_240p) {
        x0 = (float)((320 - machine.width) / 2);  x1 = x0 + machine.width;
        y0 = (float)((240 - machine.height) / 2); y1 = y0 + machine.height;
    } else if (machine.fill_screen) {
        x0 = 0.0f; x1 = 640.0f; y0 = 0.0f; y1 = 480.0f;
    } else {
        float w = machine.width * 2.0f, h = machine.height * 2.0f;
        x0 = (640.0f - w) / 2.0f; x1 = x0 + w;
        y0 = (480.0f - h) / 2.0f; y1 = y0 + h;
    }
    const float u1 = (float)machine.width / machine.tex_w;
    const float v1 = (float)machine.height / machine.tex_h;
    pvr_vertex_t v;

    pvr_prim((void *)hdr, sizeof(*hdr));
    v.argb = argb; v.oargb = 0; v.z = 1.0f;

    v.flags = PVR_CMD_VERTEX;     v.x = x0; v.y = y1; v.u = 0;  v.v = v1; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX;     v.x = x0; v.y = y0; v.u = 0;  v.v = 0;  pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX;     v.x = x1; v.y = y1; v.u = u1; v.v = v1; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL; v.x = x1; v.y = y0; v.u = u1; v.v = 0;  pvr_prim(&v, sizeof(v));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint8_t *data[MAX_FILES] = { 0 };
    uint32_t sizes[MAX_FILES] = { 0 };
    for (int i = 0; i < machine.nfiles && i < MAX_FILES; i++)
        data[i] = load(&machine.files[i], &sizes[i]);

    audio_rate = machine.audio_rate ? (uint32_t)machine.audio_rate : 44100;
    machine.init(data, sizes);

    snd_stream_init();
    snd_stream_hnd_t snd = snd_stream_alloc(audio_cb, AUDIO_BUF);
    if (snd == SND_STREAM_INVALID)
        fatal("snd_stream_alloc failed");

    if (machine.video_240p)
        vid_set_mode(DM_320x240, PM_RGB565);
    if (machine.pvr_draw) {
        /* hardware rendering: punch-through list and room for a few
           thousand textured rectangles per frame */
        pvr_init_params_t params = {
            .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_32 },
            .vertex_buf_size = 1536 * 1024,       /* 9000 rectangles with a header each */
            .opb_overflow_count = 4
        };
        pvr_init(&params);
    } else {
        pvr_init_defaults();
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    size_t fb_bytes = (size_t)machine.tex_w * machine.height * 2;
    fb = memalign(32, fb_bytes);
    pvr_ptr_t texs[2];
    texs[0] = pvr_mem_malloc(machine.tex_w * machine.tex_h * 2);
    texs[1] = machine.interframe_blend ? pvr_mem_malloc(machine.tex_w * machine.tex_h * 2) : NULL;
    if (!fb || !texs[0] || (machine.interframe_blend && !texs[1]))
        fatal("out of memory");
    memset(fb, 0, fb_bytes);
    pvr_txr_load(fb, texs[0], fb_bytes);
    if (texs[1]) pvr_txr_load(fb, texs[1], fb_bytes);
    blend_on = machine.interframe_blend && !machine.video_240p;   /* 240p shows real flicker */
    if (machine.pvr_draw) {
        if (machine.pvr_init && machine.pvr_init() != 0)
            fatal("out of video memory");
        hw_render = 1;
        blend_on = 0;
    }
    int cur = 0;
    pvr_ptr_t tex = texs[0];
    uint64_t last_drawn = 0;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     machine.tex_w, machine.tex_h, tex,
                     (machine.bilinear && !machine.video_240p) ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
    pvr_poly_compile(&hdr, &cxt);
    /* blending: opaque + translucent header for each texture */
    pvr_poly_hdr_t hdr_op[2], hdr_tr[2];
    for (int i = 0; i < 2 && machine.interframe_blend; i++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         machine.tex_w, machine.tex_h, texs[i],
                         (machine.bilinear && !machine.video_240p) ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        pvr_poly_compile(&hdr_op[i], &cxt);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                         machine.tex_w, machine.tex_h, texs[i],
                         (machine.bilinear && !machine.video_240p) ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        pvr_poly_compile(&hdr_tr[i], &cxt);
    }

    snd_stream_start(snd, audio_rate, machine.audio_stereo);

    int quit = 0;

    /* Speed control: the machine always advances one 1/60 s frame per loop.
       When the loop falls behind real time (video too slow), the drawing of
       that frame is skipped so the game keeps its speed; at most MAX_SKIP in
       a row. Timing follows the NTSC field rate (59.94 Hz). */
    #define MAX_SKIP 3
    uint64_t t_base = timer_us_gettime64();
    uint64_t emu_frames = 0;
    int skipped = 0;
#ifdef NGCD_PERF
    /* timing stats, printed to the debug console every 2 s */
    uint64_t t_emu = 0, t_vid = 0, t_wait = 0, t_up = 0;
    int frames = 0, redraws = 0, shown = 0;
#endif

    while (!quit) {
#ifdef NGCD_PERF
        uint64_t t0 = timer_us_gettime64();
#endif
        poll_pad(&quit);
        machine.run_frame();
        snd_stream_poll(snd);
        uint64_t t1 = timer_us_gettime64();

        emu_frames++;
        uint64_t due = (t1 - t_base) * 5994 / 100000000;   /* frames due at 59.94 Hz */
        if (due > emu_frames + 30) {                         /* long stall: resync */
            t_base = t1 - emu_frames * 100000000 / 5994;
            due = emu_frames;
        }
        if (emu_frames < due && skipped < MAX_SKIP) {
            skipped++;
#ifdef NGCD_PERF
            t_emu += t1 - t0;
            if (++frames == 120) {
                dbglog(DBG_INFO, "perf/frame: emu %u us, video %u us, wait %u us, redraws %d/120, shown %d/120\n",
                       (unsigned)(t_emu / 120), shown ? (unsigned)(t_vid / shown) : 0,
                       shown ? (unsigned)(t_wait / shown) : 0, redraws, shown);
                t_emu = t_vid = t_wait = 0;
                frames = redraws = shown = 0;
            }
#endif
            continue;
        }
        skipped = 0;

        if (hw_render) {
            /* the machine draws with the PVR itself */
            pvr_wait_ready();
#ifdef NGCD_PERF
            uint64_t tw = timer_us_gettime64();
#endif
            pvr_scene_begin();
            machine.pvr_draw();
            pvr_scene_finish();
#ifdef NGCD_PERF
            uint64_t t3 = timer_us_gettime64();
            t_emu += t1 - t0; t_vid += t3 - tw; t_wait += tw - t1;
            shown++; redraws++;
            if (++frames == 120) {
                dbglog(DBG_INFO, "perf/frame (pvr): emu %u us, draw %u us, wait %u us, shown %d/120\n",
                       (unsigned)(t_emu / 120), (unsigned)(t_vid / shown), (unsigned)(t_wait / shown), shown);
                t_emu = t_vid = t_wait = t_up = 0;
                frames = redraws = shown = 0;
            }
#endif
            continue;
        }
        uint64_t rows = machine.render_dirty(fb, machine.tex_w);
#ifdef NGCD_PERF
        uint64_t tu = timer_us_gettime64();
#endif
        /* the texture about to be written may still be used by the scene
           the PVR is rendering: wait for it first */
        pvr_wait_ready();                     /* paces the loop to the display */
        if (blend_on) {
            /* alternate textures; the new one needs the whole picture */
            cur ^= 1;
            tex = texs[cur];
            rows = (1ull << (machine.height / 8)) - 1;
        } else if (machine.interframe_blend && tex != texs[0]) {
            cur = 0; tex = texs[0];
            rows = (1ull << (machine.height / 8)) - 1;
        }
        if (rows) {
            upload_rows(tex, rows);
#ifdef NGCD_PERF
            redraws++;
#endif
        }
#ifdef NGCD_PERF
        uint64_t t2 = timer_us_gettime64();
        t_up += t2 - tu;
#endif

        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_screen(machine.interframe_blend ? &hdr_op[cur] : &hdr, 0xFFFFFFFF);
        pvr_list_finish();
        /* blend only consecutive machine frames: across skipped frames the
           previous picture is several frames old and leaves a trail */
        int blend_now = blend_on && emu_frames == last_drawn + 1;
        last_drawn = emu_frames;
        if (blend_now) {
            pvr_list_begin(PVR_LIST_TR_POLY);
            draw_screen(&hdr_tr[cur ^ 1], 0x80FFFFFF);
            pvr_list_finish();
        }
        pvr_scene_finish();

#ifdef NGCD_PERF
        uint64_t t3 = timer_us_gettime64();
        t_emu += t1 - t0; t_vid += t2 - t1; t_wait += t3 - t2;
        shown++;
        if (++frames == 120) {
            /* emu: average per machine frame; video/wait: per displayed frame */
            dbglog(DBG_INFO, "perf/frame: emu %u us, video %u us (upload %u), wait %u us, redraws %d/120, shown %d/120\n",
                   (unsigned)(t_emu / 120), (unsigned)(t_vid / shown), (unsigned)(t_up / shown),
                   (unsigned)(t_wait / shown), redraws, shown);
            t_emu = t_vid = t_wait = t_up = 0;
            frames = redraws = shown = 0;
        }
#endif
    }

    snd_stream_stop(snd);
    snd_stream_destroy(snd);
    snd_stream_shutdown();
    pvr_shutdown();
    return 0;
}

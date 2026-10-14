/* ngcd-dc: Dreamcast / KallistiOS frontend (main loop, video, pads, audio).
   Drives the machine described by machine.h (machine_ngcd.c).
   Video is 320x240 progressive; the machine draws each frame with the PVR
   itself (video_pvr.c), there is no software picture. */
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <arch/timer.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"

#ifdef NGCD_RELEASE
/* INIT_QUIET disables dbgio entirely: no banner, no dbglog output */
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_QUIET);
#else
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

#define AUDIO_BUF   8192            /* bytes, largest callback request */
/* AICA ring per channel: 2048 samples (46 ms at 44.1 kHz), refilled half at
   a time, so what is written plays 23-46 ms later */
#define STREAM_BYTES 4096
#define MAX_FILES   16

static int16_t   abuf[AUDIO_BUF / 2] __attribute__((aligned(32)));

/* The game folder. An anthology menu (menu/menu.c) loads this program as a
   plain binary, finds the marker below in the image and writes the folder
   after it ("/cd/SSII/") before starting it. Started directly, the game
   is in the disc's root. 64 bytes in .data, so the marker is in the
   binary. */
#define DIR_MARKER "NGCD-DISC-DIR="
volatile char disc_dir_patch[64] __attribute__((used, aligned(4))) = DIR_MARKER "/cd/";
const char *disc_dir = "/cd/";
static char disc_dir_buf[48];

static void pick_disc_dir(void) {
    const volatile char *p = disc_dir_patch + sizeof(DIR_MARKER) - 1;
    size_t n = 0;
    while (n < sizeof(disc_dir_buf) - 2 && p[n] && p[n] != ' ') { disc_dir_buf[n] = p[n]; n++; }
    disc_dir_buf[n] = 0;
    if (n < 5 || strncmp(disc_dir_buf, "/cd/", 4) != 0) return;
    if (disc_dir_buf[n - 1] != '/') { disc_dir_buf[n++] = '/'; disc_dir_buf[n] = 0; }
    disc_dir = disc_dir_buf;
}
static uint32_t  audio_rate;

static void fatal(const char *msg) {
    dbglog(DBG_ERROR, "%s: %s\n", machine.title, msg);
    vid_clear(0, 0, 64);
    bfont_draw_str(vram_s + 24 * vid_mode->width + 8, vid_mode->width, true, msg);
    for (;;)
        thd_sleep(1000);
}

static uint8_t *load(const mach_file_t *mf, uint32_t *out_size) {
    char path[96], m[128];
    /* the game folder first, then the disc's root (one BIOS for all games) */
    snprintf(path, sizeof(path), "%s%s", disc_dir, mf->name);
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID && strcmp(disc_dir, "/cd/") != 0) {
        snprintf(path, sizeof(path), "/cd/%s", mf->name);
        f = fs_open(path, O_RDONLY);
    }
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

/* The stream is refilled by its own thread every ~5 ms, not by the main
   loop: when the main loop stalls (a disc read after a menu choice), the
   AICA would otherwise loop over its ring and replay the last sounds
   several times. With nothing new from the machine the callback holds the
   last sample. The SQ users (PVR lists, snd_stream) share KOS' SQ mutex. */
static volatile int audio_run;
static void *audio_thread(void *arg) {
    snd_stream_hnd_t snd = (snd_stream_hnd_t)(intptr_t)arg;
    while (audio_run) {
        snd_stream_poll(snd);
        thd_sleep(5);
    }
    return NULL;
}

/* every controller goes to the machine (port 0..3, missing = buttons 0);
   on the first one Start+A+B+X+Y quits */
static void poll_pad(int *quit) {
    for (int p = 0; p < 4; p++) {
        maple_device_t *d = maple_enum_type(p, MAPLE_FUNC_CONTROLLER);
        cont_state_t *s = d ? (cont_state_t *)maple_dev_status(d) : NULL;
        if (!s) { machine.set_pad_n(p, 0, 0, 0); continue; }
        machine.set_pad_n(p, s->buttons, s->ltrig, s->rtrig);
        if (p == 0) {
            const uint32_t exit_combo = CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y;
            if ((s->buttons & exit_combo) == exit_combo) *quit = 1;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    pick_disc_dir();
    dbglog(DBG_INFO, "%s: game folder %s\n", machine.title, disc_dir);

    uint8_t *data[MAX_FILES] = { 0 };
    uint32_t sizes[MAX_FILES] = { 0 };
    for (int i = 0; i < machine.nfiles && i < MAX_FILES; i++)
        data[i] = load(&machine.files[i], &sizes[i]);

    audio_rate = machine.audio_rate ? (uint32_t)machine.audio_rate : 44100;
    machine.init(data, sizes);

    snd_stream_init();
    snd_stream_hnd_t snd = snd_stream_alloc(audio_cb, STREAM_BYTES);
    if (snd == SND_STREAM_INVALID)
        fatal("snd_stream_alloc failed");

    vid_set_mode(DM_320x240, PM_RGB565);
    /* opaque + punch-through lists, room for 9000 textured rectangles
       with a header each */
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_32 },
        .vertex_buf_size = 1536 * 1024,
        .opb_overflow_count = 4
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    if (machine.pvr_init() != 0)
        fatal("out of video memory");

    snd_stream_start(snd, audio_rate, machine.audio_stereo);
    audio_run = 1;
    kthread_t *athd = thd_create(0, audio_thread, (void *)(intptr_t)snd);
    if (!athd) fatal("audio thread");
    thd_set_prio(athd, PRIO_DEFAULT - 1);         /* before the emulation */

    int quit = 0;

    /* Speed control: the machine always advances one 1/60 s frame per loop.
       When the loop falls behind real time, the drawing of that frame is
       skipped so the game keeps its speed; at most MAX_SKIP in a row.
       Timing follows the NTSC field rate (59.94 Hz). */
    #define MAX_SKIP 3
    uint64_t t_base = timer_us_gettime64();
    uint64_t emu_frames = 0;
    int skipped = 0;
#ifdef NGCD_PERF
    /* timing stats, printed to the debug console every 120 machine frames:
       emu per machine frame, draw / wait per shown frame */
    uint64_t t_emu = 0, t_draw = 0, t_wait = 0;
    int frames = 0, shown = 0;
#endif

    while (!quit) {
#ifdef NGCD_PERF
        uint64_t t0 = timer_us_gettime64();
#endif
        poll_pad(&quit);
        machine.run_frame();
        uint64_t t1 = timer_us_gettime64();
#ifdef NGCD_PERF
        t_emu += t1 - t0;
#endif

        emu_frames++;
        uint64_t due = (t1 - t_base) * 5994 / 100000000;   /* frames due at 59.94 Hz */
        if (due > emu_frames + 30) {                         /* long stall: resync */
            t_base = t1 - emu_frames * 100000000 / 5994;
            due = emu_frames;
        }
        if (emu_frames < due && skipped < MAX_SKIP) {
            skipped++;
        } else {
            skipped = 0;
            pvr_wait_ready();                 /* paces the loop to the display */
#ifdef NGCD_PERF
            uint64_t tw = timer_us_gettime64();
#endif
            pvr_scene_begin();
            machine.pvr_draw();
            pvr_scene_finish();
#ifdef NGCD_PERF
            t_draw += timer_us_gettime64() - tw;
            t_wait += tw - t1;
            shown++;
#endif
        }
#ifdef NGCD_PERF
        if (++frames == 120) {
            dbglog(DBG_INFO, "perf/frame: emu %u us, draw %u us, wait %u us, shown %d/120\n",
                   (unsigned)(t_emu / 120), shown ? (unsigned)(t_draw / shown) : 0,
                   shown ? (unsigned)(t_wait / shown) : 0, shown);
            t_emu = t_draw = t_wait = 0;
            frames = shown = 0;
        }
#endif
    }

    audio_run = 0;
    thd_join(athd, NULL);
    snd_stream_stop(snd);
    snd_stream_destroy(snd);
    snd_stream_shutdown();

    pvr_shutdown();
    return 0;
}

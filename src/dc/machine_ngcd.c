/* ngcd-dc: Neo Geo CD (CDZ) machine for the Dreamcast frontend.
   Runs the real CDZ BIOS (NEOCD.BIN) on the ngcd core; the game disc is
   converted by tools/ngcdconv into files in the game folder (the disc's
   root, or e.g. /cd/SSII/ when started from the anthology menu):

     NEOCD.BIN    CDZ BIOS (512 KB, required; the folder or the root)
     NG_LO.ROM    Y zoom table 000-lo.lo (64 KB, optional - generated if missing)
     TOC.BIN      track table
     DATA.ISO     data track (2048-byte sectors)
     TRKnn.NGA    CD-DA tracks as IMA ADPCM, 44.1 kHz stereo
     NGCD.CFG     optional settings

   68000 on FAME SH4, Z80 on RAZE-SH4. The YM2610 runs at 22050 Hz and is
   upsampled to the 44100 Hz stereo stream the CD audio is mixed into.
   CD audio is streamed by a reader thread. The 8 KB backup RAM is kept on
   the first VMU (NGCDBAK). */
#include <kos.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dc/maple/controller.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include "machine.h"
#include "../core/ngcd.h"
#include "../core/nga.h"

/* drawing with the PVR (video_pvr.c) */
int  ngcd_pvr_init(void);
void ngcd_pvr_draw(float ox, float oy);
extern uint32_t ngcd_pvr_tiles_made, ngcd_pvr_quads, ngcd_pvr_overflow, ngcd_pvr_dropped;
extern uint32_t ngcd_raster_frames;           /* video.c */

#define OUT_RATE  44100

enum { F_BIOS, F_LO, F_TOC, NFILES };

static const mach_file_t files[] = {
    { "NEOCD.BIN",  0x80000, 0, 1 },
    { "NG_LO.ROM",  0x10000, 0, 0 },   /* 000-lo.lo */
    { "TOC.BIN",    0,       0, 1 },
};

static void die(const char *m) {
    dbglog(DBG_ERROR, "ngcd: %s\n", m);
    vid_clear(0, 0, 64);
    bfont_draw_str(vram_s + 24 * vid_mode->width + 8, vid_mode->width, true, m);
    for (;;) thd_sleep(1000);
}

/* ------------------------------------------------------------ data track */
#define CACHE_SECTORS 32                     /* 64 KB read-ahead */
static file_t iso = FILEHND_INVALID;
static uint32_t iso_sectors;
static uint8_t cache[CACHE_SECTORS * 2048] __attribute__((aligned(32)));
static uint32_t cache_first = 0xFFFFFFFF, cache_count;

static int rd_sector(uint32_t s, uint8_t *buf) {
    if (s >= iso_sectors) { memset(buf, 0, 2048); return 0; }
    if (s < cache_first || s >= cache_first + cache_count) {
        uint32_t n = iso_sectors - s;
        if (n > CACHE_SECTORS) n = CACHE_SECTORS;
        if (fs_seek(iso, (off_t)s * 2048, SEEK_SET) < 0 ||
            fs_read(iso, cache, n * 2048) != (ssize_t)(n * 2048)) {
            cache_first = 0xFFFFFFFF; cache_count = 0;
            memset(buf, 0, 2048);
            return 0;
        }
        cache_first = s; cache_count = n;
    }
    memcpy(buf, cache + (s - cache_first) * 2048, 2048);
    return 1;
}

/* ------------------------------------------------------------ CD audio */
#define RING_BLOCKS 64                       /* 0.85 s */
#define READ_BATCH  8
static uint8_t ring[RING_BLOCKS][NGA_BLOCK];
static volatile uint32_t ring_head, ring_tail;   /* blocks written / consumed */
static mutex_t ring_lock;
static volatile int req_gen, req_track, req_play;
static volatile int32_t req_offset;
static int silence_blocks;                   /* pregap before the track (consumer) */
static int16_t cd_pcm[NGA_SAMPLES * 2];
static int cd_pos = NGA_SAMPLES;
static uint32_t cd_underruns;

static void au_play(int track, int32_t off) {
    mutex_lock(&ring_lock);
    req_track = track;
    req_offset = off < 0 ? 0 : off;
    silence_blocks = off < 0 ? -off : 0;
    req_play = 1;
    req_gen++;
    ring_head = ring_tail = 0;
    cd_pos = NGA_SAMPLES;
    mutex_unlock(&ring_lock);
}

static void au_stop(void) {
    mutex_lock(&ring_lock);
    req_play = 0;
    req_gen++;
    ring_head = ring_tail = 0;
    silence_blocks = 0;
    cd_pos = NGA_SAMPLES;
    mutex_unlock(&ring_lock);
}

static void *reader_thread(void *arg) {
    (void)arg;
    static uint8_t tmp[READ_BATCH * NGA_BLOCK];
    file_t f = FILEHND_INVALID;
    int gen = -1, play = 0;
    uint32_t sector = 0, nsectors = 0;
    for (;;) {
        mutex_lock(&ring_lock);
        int g = req_gen, want = req_play, track = req_track;
        int32_t off = req_offset;
        uint32_t room = RING_BLOCKS - (ring_head - ring_tail);
        mutex_unlock(&ring_lock);

        if (g != gen) {                       /* new request */
            gen = g;
            if (f != FILEHND_INVALID) { fs_close(f); f = FILEHND_INVALID; }
            play = 0;
            if (want) {
                char p[96];
                uint8_t h[NGA_HEADER];
                snprintf(p, sizeof p, "%sTRK%02d.NGA", disc_dir, track);
                f = fs_open(p, O_RDONLY);
                if (f != FILEHND_INVALID && fs_read(f, h, NGA_HEADER) == NGA_HEADER &&
                    !memcmp(h, "NGA1", 4)) {
                    nsectors = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
                    sector = (uint32_t)off;
                    if (sector < nsectors &&
                        fs_seek(f, NGA_HEADER + (off_t)sector * NGA_BLOCK, SEEK_SET) >= 0)
                        play = 1;
                } else {
                    dbglog(DBG_WARNING, "ngcd: cannot play %s\n", p);
                }
            }
            continue;
        }
        if (!play || room < READ_BATCH) { thd_sleep(10); continue; }

        uint32_t n = nsectors - sector;
        if (n > READ_BATCH) n = READ_BATCH;
        if (n == 0 || fs_read(f, tmp, n * NGA_BLOCK) != (ssize_t)(n * NGA_BLOCK)) {
            play = 0;                         /* end of track */
            continue;
        }
        sector += n;
        mutex_lock(&ring_lock);
        if (req_gen == gen) {
            for (uint32_t i = 0; i < n; i++)
                memcpy(ring[(ring_head + i) % RING_BLOCKS], tmp + i * NGA_BLOCK, NGA_BLOCK);
            ring_head += n;
        }
        mutex_unlock(&ring_lock);
    }
    return NULL;
}

/* next CD sample pair; returns 0 when silent */
static inline void cd_next(int *l, int *r) {
    if (cd_pos >= NGA_SAMPLES) {
        static uint8_t blk[NGA_BLOCK];
        int have = 0;
        mutex_lock(&ring_lock);
        if (silence_blocks > 0) {
            silence_blocks--;
            memset(cd_pcm, 0, sizeof cd_pcm);
            cd_pos = 0;
            mutex_unlock(&ring_lock);
            goto out;
        }
        if (ring_head != ring_tail) {
            memcpy(blk, ring[ring_tail % RING_BLOCKS], NGA_BLOCK);
            ring_tail++;
            have = 1;
        } else if (req_play) {
            cd_underruns++;
        }
        mutex_unlock(&ring_lock);
        if (!have) { *l = *r = 0; return; }
        nga_decode_block(blk, cd_pcm);
        cd_pos = 0;
    }
out:
    *l = cd_pcm[2 * cd_pos];
    *r = cd_pcm[2 * cd_pos + 1];
    cd_pos++;
}

static const ngcd_disc_io_t disc_io = { rd_sector, au_play, au_stop };

/* ------------------------------------------------------------ backup RAM */
#define VMU_FILE "NGCDBAK"
static uint8_t saved_backup[NGCD_BACKUP_SIZE];
static uint8_t save_buf[NGCD_BACKUP_SIZE];
static volatile int saving;
static uint32_t backup_check_frames;
static int backup_dirty_checks;

static maple_device_t *first_vmu(void) {
    return maple_enum_type(0, MAPLE_FUNC_MEMCARD);
}

static void backup_load(void) {
    maple_device_t *vmu = first_vmu();
    void *data = NULL;
    int size = 0;
    if (vmu && vmufs_read(vmu, VMU_FILE, &data, &size) == 0 && data) {
        /* VMU file header (0x80 bytes): icon_cnt @0x40, eyecatch @0x44,
           data_len @0x48; icons (512 bytes each) follow, then the payload */
        const uint8_t *h = data;
        if (size >= 0x80) {
            uint32_t icons = h[0x40] | (h[0x41] << 8);
            uint32_t len = h[0x48] | (h[0x49] << 8) | (h[0x4A] << 16) | ((uint32_t)h[0x4B] << 24);
            uint32_t off = 0x80 + icons * 512;
            if (h[0x44] == 0 && len == NGCD_BACKUP_SIZE && off + len <= (uint32_t)size) {
                memcpy(ng.backup, h + off, NGCD_BACKUP_SIZE);
                dbglog(DBG_INFO, "ngcd: backup RAM loaded from VMU\n");
            }
        }
        free(data);
    }
    memcpy(saved_backup, ng.backup, NGCD_BACKUP_SIZE);
}

static void *save_thread(void *arg) {
    (void)arg;
    maple_device_t *vmu = first_vmu();
    if (vmu) {
        vmu_pkg_t pkg;
        memset(&pkg, 0, sizeof pkg);
        strcpy(pkg.desc_short, "Neo Geo CD save");
        strcpy(pkg.desc_long, "Neo Geo CD backup RAM");
        strcpy(pkg.app_id, "ngcd-dc");
        pkg.icon_cnt = 0;
        pkg.eyecatch_type = VMUPKG_EC_NONE;
        pkg.data_len = NGCD_BACKUP_SIZE;
        pkg.data = save_buf;
        uint8_t *out = NULL;
        int out_size = 0;
        if (vmu_pkg_build(&pkg, &out, &out_size) == 0 && out) {
            if (vmufs_write(vmu, VMU_FILE, out, out_size, VMUFS_OVERWRITE) == 0)
                dbglog(DBG_INFO, "ngcd: backup RAM saved to VMU\n");
            else
                dbglog(DBG_WARNING, "ngcd: VMU write failed\n");
            free(out);
        }
    }
    saving = 0;
    return NULL;
}

/* once a second: save when the backup RAM changed and then stayed the same */
static void backup_poll(void) {
    if (++backup_check_frames < 60) return;
    backup_check_frames = 0;
    static uint8_t last[NGCD_BACKUP_SIZE];
    int changed = memcmp(ng.backup, saved_backup, NGCD_BACKUP_SIZE) != 0;
    int stable = memcmp(ng.backup, last, NGCD_BACKUP_SIZE) == 0;
    memcpy(last, ng.backup, NGCD_BACKUP_SIZE);
    if (!changed) { backup_dirty_checks = 0; return; }
    if (!stable) { backup_dirty_checks = 0; return; }
    if (++backup_dirty_checks < 2 || saving) return;
    backup_dirty_checks = 0;
    memcpy(save_buf, ng.backup, NGCD_BACKUP_SIZE);
    memcpy(saved_backup, ng.backup, NGCD_BACKUP_SIZE);
    saving = 1;
    thd_create(1, save_thread, NULL);
}

/* ------------------------------------------------------------ machine */
static ngcd_toc_t toc;

static void parse_toc(const uint8_t *b, uint32_t size) {
    if (size < 20 || memcmp(b, "NGTOC1", 6)) die("TOC.BIN: bad file (use tools/ngcdconv)");
    toc.n = b[8] | (b[9] << 8);
    if (toc.n > 200 || size < 20 + (uint32_t)toc.n * 12) die("TOC.BIN: bad size");
    toc.total = b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t)b[15] << 24);
    toc.first = b[16]; toc.last = b[17];
    for (int i = 0; i < toc.n; i++) {
        const uint8_t *e = b + 20 + i * 12;
        toc.e[i].track = e[0]; toc.e[i].index = e[1]; toc.e[i].is_data = e[2];
        toc.e[i].start  = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
        toc.e[i].length = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
    }
}

/* optional NGCD.CFG in the game folder, lines "key=value":
     idle=0|1             skip the 68000's wait loops (default 1)
     cpu=12|15|18|21|24   68000 clock in MHz (12 = real hardware). More
                          makes games that slow down in heavy scenes (as
                          on the real console) slow down less, at a cost
                          in Dreamcast CPU time.
     region=jp|us|eu      the console's region (default jp): the BIOS and
                          the games show that region's title, language and
                          content (e.g. Samurai Spirits / Samurai Shodown)
     video=320|640        screen mode: 320 = 320x240 (default; 240p on a TV,
                          line doubled on VGA), 640 = 640x480 with every
                          pixel drawn 2x2 (VGA and emulators; 480i on a TV)
     stick=0|1            the analog stick as the joystick (default 1) */
static int cfg_region = 0, cfg_scale = 1, cfg_stick = 1;

static int cfg_is(const char *v, const char *a, const char *b, const char *c) {
    size_t n = strcspn(v, " \t\r");
    return (n == strlen(a) && !strncasecmp(v, a, n)) || (b && n == strlen(b) && !strncasecmp(v, b, n))
        || (c && n == strlen(c) && !strncasecmp(v, c, n));
}

static void load_config(void) {
    char path[96];
    snprintf(path, sizeof path, "%sNGCD.CFG", disc_dir);
    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) return;
    char buf[1024];
    ssize_t n = fs_read(f, buf, sizeof(buf) - 1);
    fs_close(f);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *l = buf; l && *l; ) {
        char *next = strchr(l, '\n');
        if (next) *next++ = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "idle=", 5)) {
            ngcd_idle_skip = atoi(l + 5) != 0;
            dbglog(DBG_INFO, "ngcd: wait loop skipping %s\n", ngcd_idle_skip ? "on" : "off");
        }
        if (!strncmp(l, "cpu=", 4)) {
            int mhz = atoi(l + 4);
            if (mhz >= 12 && mhz <= 24) {
                ngcd_cpu_clock8 = mhz / 3;               /* 1/8 of the 24 MHz master */
                dbglog(DBG_INFO, "ngcd: 68000 at %d MHz\n", ngcd_cpu_clock8 * 3);
            }
        }
        if (!strncmp(l, "region=", 7)) {
            const char *v = l + 7;
            if (cfg_is(v, "jp", "japan", "0")) cfg_region = 0;
            else if (cfg_is(v, "us", "usa", "1")) cfg_region = 1;
            else if (cfg_is(v, "eu", "europe", "2")) cfg_region = 2;
            dbglog(DBG_INFO, "ngcd: region %s\n", cfg_region == 0 ? "Japan" : cfg_region == 1 ? "USA" : "Europe");
        }
        if (!strncmp(l, "video=", 6)) {
            cfg_scale = atoi(l + 6) == 640 ? 2 : 1;
            dbglog(DBG_INFO, "ngcd: screen %s\n", cfg_scale == 2 ? "640x480" : "320x240");
        }
        if (!strncmp(l, "stick=", 6))
            cfg_stick = atoi(l + 6) != 0;
        l = next;
    }
}

static void m_init(uint8_t *const *d, const uint32_t *sz) {
    load_config();
    parse_toc(d[F_TOC], sz[F_TOC]);
    char path[96];
    snprintf(path, sizeof path, "%sDATA.ISO", disc_dir);
    iso = fs_open(path, O_RDONLY);
    if (iso == FILEHND_INVALID) die("Missing DATA.ISO");
    iso_sectors = (uint32_t)(fs_total(iso) / 2048);

    mutex_init(&ring_lock, MUTEX_TYPE_NORMAL);
    if (ngcd_init(d[F_BIOS], d[F_LO], &toc, &disc_io) != 0) die("out of memory");
    ng.nationality = (uint32_t)cfg_region;    /* read by the BIOS from $FF011C */
    free(d[F_BIOS]); free(d[F_LO]); free(d[F_TOC]);
    backup_load();
    thd_create(1, reader_thread, NULL);
}

/* Neo Geo pad: U D L R A B C D, active low; system: start/select P1, P2 */
static uint8_t pad[2], sys = 0x0F;

static uint8_t map_pad(uint32_t b) {
    uint8_t v = 0xFF;
    if (b & CONT_DPAD_UP)    v &= ~0x01;
    if (b & CONT_DPAD_DOWN)  v &= ~0x02;
    if (b & CONT_DPAD_LEFT)  v &= ~0x04;
    if (b & CONT_DPAD_RIGHT) v &= ~0x08;
    if (b & CONT_A)          v &= ~0x10;
    if (b & CONT_B)          v &= ~0x20;
    if (b & CONT_X)          v &= ~0x40;
    if (b & CONT_Y)          v &= ~0x80;
    return v;
}

static void m_set_pad_n(int port, uint32_t b, int lt, int rt, int jx, int jy) {
    (void)rt;
    if (port > 1) return;
    if (cfg_stick) {                          /* the stick as the joystick, past a dead zone */
        if (jx < -64) b |= CONT_DPAD_LEFT;
        if (jx > 64)  b |= CONT_DPAD_RIGHT;
        if (jy < -64) b |= CONT_DPAD_UP;
        if (jy > 64)  b |= CONT_DPAD_DOWN;
    }
    if ((b & CONT_DPAD_LEFT) && (b & CONT_DPAD_RIGHT)) b &= ~(CONT_DPAD_LEFT | CONT_DPAD_RIGHT);
    if ((b & CONT_DPAD_UP) && (b & CONT_DPAD_DOWN)) b &= ~(CONT_DPAD_UP | CONT_DPAD_DOWN);
    pad[port] = map_pad(b);
    uint8_t start = port ? 0x04 : 0x01, select = port ? 0x08 : 0x02;
    if (b & CONT_START) sys &= ~start; else sys |= start;
    if (lt > 128)       sys &= ~select; else sys |= select;
}

static uint32_t ym_underruns;

#ifdef NGCD_PERF
#include <arch/timer.h>
extern uint64_t ngcd_prof_ym, ngcd_prof_z80;
uint64_t ngcd_prof_clock(void) { return timer_ns_gettime64(); }
static uint64_t prof_emu, prof_render, prof_audio;
static uint32_t prof_renders;
static uint32_t prof_frames, prof_sectors;
#endif

static void m_run_frame(void) {
#ifdef NGCD_PERF
    uint64_t t0 = timer_us_gettime64();
#endif
    ngcd_set_input(pad[0], pad[1], sys);
    ngcd_run_frame();
    backup_poll();
#ifdef NGCD_PERF
    prof_emu += timer_us_gettime64() - t0;
    if (++prof_frames == 120) {
        dbglog(DBG_INFO, "ngcd/frame us: emu %u (z80 %u, ym %u), draw %u (per shown frame), audio %u; CD sectors %u, cdda underruns %u, ym underruns %u; 68k %d MHz, wait slices skipped %u\n",
               (unsigned)(prof_emu / 120), (unsigned)(ngcd_prof_z80 / 120000), (unsigned)(ngcd_prof_ym / 120000),
               prof_renders ? (unsigned)(prof_render / prof_renders) : 0, (unsigned)(prof_audio / 120),
               (unsigned)(ng.sectors_read - prof_sectors), (unsigned)cd_underruns, (unsigned)ym_underruns,
               ngcd_cpu_clock8 * 3, (unsigned)ngcd_idle_slices);
        ngcd_idle_slices = 0;
        if (ngcd_pvr_quads)
            dbglog(DBG_INFO, "ngcd/pvr: %u rects/frame, %u tiles converted, %u cache overflows, %u rects dropped, %u raster frames\n",
                   (unsigned)(ngcd_pvr_quads / (prof_renders ? prof_renders : 1)),
                   (unsigned)ngcd_pvr_tiles_made, (unsigned)ngcd_pvr_overflow, (unsigned)ngcd_pvr_dropped,
                   (unsigned)ngcd_raster_frames);
        ngcd_pvr_quads = ngcd_pvr_tiles_made = ngcd_pvr_overflow = ngcd_pvr_dropped = 0;
        ngcd_raster_frames = 0;
        prof_emu = prof_render = prof_audio = 0;
        prof_renders = 0;
        ngcd_prof_ym = ngcd_prof_z80 = 0;
        prof_frames = 0;
        prof_sectors = ng.sectors_read;
    }
#endif
}

static int m_pvr_init(void) { return ngcd_pvr_init(); }
static int m_video_scale(void) { return cfg_scale; }
extern float ngcd_pvr_scale;
static void m_pvr_draw(void) {
#ifdef NGCD_PERF
    uint64_t t0 = timer_us_gettime64();
#endif
    /* 240p, picture 1:1; or 640x480 with every pixel 2x2 */
    ngcd_pvr_scale = (float)cfg_scale;
    ngcd_pvr_draw(0.0f, (float)((240 - NGCD_FB_H) / 2 * cfg_scale));
#ifdef NGCD_PERF
    prof_render += timer_us_gettime64() - t0;
    prof_renders++;
#endif
}

/* YM2610 at 22050 Hz -> 44100 Hz with linear interpolation, + CD audio.
   The machine runs at 59.6 Hz but is driven at 59.94 Hz, so it produces
   slightly more YM samples than the output plays: instead of dropping
   chunks (clicks) the resampling step follows the backlog smoothly. */
static int16_t ym_buf[1024 * 2];
static int ym_have, ym_pos;
static int ym_prev_l, ym_prev_r, ym_cur_l, ym_cur_r;
static uint32_t ym_frac;                      /* 16.16 position between prev and cur */
static int32_t err_f;                         /* 16 x smoothed backlog error */
#define YM_STEP_NOMINAL 32954u                /* 65536 * 22050/44100 * 59.94/59.6 */

static inline void ym_next(void) {
    ym_prev_l = ym_cur_l; ym_prev_r = ym_cur_r;
    if (ym_pos >= ym_have) {
        ym_have = ngcd_audio_take(ym_buf, 1024);
        ym_pos = 0;
        if (!ym_have) { ym_underruns++; return; }   /* underrun: hold the last sample */
    }
    ym_cur_l = ym_buf[2 * ym_pos];
    ym_cur_r = ym_buf[2 * ym_pos + 1];
    ym_pos++;
}

static inline int16_t clip(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void m_audio(int16_t *out, int n, uint32_t rate) {
    (void)rate;
#ifdef NGCD_PERF
    uint64_t t0 = timer_us_gettime64();
#endif
    int frames = n / 2;
    /* backlog control: aim at what this callback uses (frames / 2 YM
       samples) + 256 (12 ms) queued; the machine adds ~370 a frame. Was
       + 1024, which with the larger AICA ring made sounds ~150 ms late. */
    int backlog = ngcd_audio_avail() + (ym_have - ym_pos);
    int target = frames / 2 + 256;
    int err = backlog - target;
    if (err > 4096) {                         /* far behind (e.g. after loading): resync */
        while (backlog > target) {
            int k = ngcd_audio_take(ym_buf, backlog - target > 1024 ? 1024 : backlog - target);
            if (!k) break;
            backlog -= k;
        }
        ym_have = ym_pos = 0;
        err = 0;
        err_f = 0;
    }
    /* the error is smoothed over many callbacks and the correction kept
       within +-0.2 % (about 3 cents): a faster loop wobbles the pitch */
    err_f += (err * 16 - err_f) / 32;         /* err_f = 16 * smoothed error */
    int32_t adj = err_f / 256;                /* +-66 at +-1050 samples */
    if (adj > 66) adj = 66;
    if (adj < -66) adj = -66;
    uint32_t step = (uint32_t)((int32_t)YM_STEP_NOMINAL + adj);

    for (int i = 0; i < frames; i++) {
        while (ym_frac >= 65536) { ym_next(); ym_frac -= 65536; }
        int f = (int)(ym_frac >> 1);          /* 0..32767 */
        int l = ym_prev_l + (((ym_cur_l - ym_prev_l) * f) >> 15);
        int r = ym_prev_r + (((ym_cur_r - ym_prev_r) * f) >> 15);
        ym_frac += step;
        int cl, cr;
        cd_next(&cl, &cr);
        out[2 * i]     = clip(l + cl);
        out[2 * i + 1] = clip(r + cr);
    }
#ifdef NGCD_PERF
    prof_audio += timer_us_gettime64() - t0;
#endif
}

const machine_t machine = {
    .title        = "ngcd-dc",
    .audio_rate   = OUT_RATE,
    .audio_stereo = 1,
    .files        = files,
    .nfiles       = NFILES,
    .init         = m_init,
    .set_pad_n    = m_set_pad_n,
    .run_frame    = m_run_frame,
    .audio        = m_audio,
    .pvr_init     = m_pvr_init,
    .video_scale  = m_video_scale,
    .pvr_draw     = m_pvr_draw,
};

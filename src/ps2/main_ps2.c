/* ngcd-ps2: the ngcd core (Neo Geo CD / CDZ) on the PlayStation 2.

   The same core as the Dreamcast build, with C CPU cores: FAME C for the
   68000 (cpu_famec.c) and superzazu's Z80 (host/z80.c). The picture is
   drawn by the core's software renderer into a 320x224 16-bit texture
   (GS CT16 palette, NGCD_PAL_GS) and shown by the GS; sound goes to
   audsrv at 44100 Hz. Files are read with stdio from the folder the ELF
   was started from (mass:/, mc0:/, hdd, host: under PCSX2 ...), as
   converted by tools/ngcdconv:

     NEOCD.BIN    CDZ BIOS (512 KB, required)
     NG_LO.ROM    Y zoom table 000-lo.lo (64 KB, optional)
     TOC.BIN      track table
     DATA.ISO     data track (2048-byte sectors)
     TRKnn.NGA    CD-DA tracks as IMA ADPCM
     NGCD.CFG     optional settings (see load_config)

   The 8 KB backup RAM is kept on the first memory card
   (mc0:/NGCDPS2/BACKUP.BIN). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <sys/stat.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <ps2_filesystem_driver.h>
#include <ps2_joystick_driver.h>
#include <ps2_audio_driver.h>
#include <libpad.h>
#include <audsrv.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <gsInline.h>
#include "ngcd.h"
#include "nga.h"

#define OUT_RATE 44100

static char base_dir[256];

/* ------------------------------------------------------------ messages */
static GSGLOBAL *gs;

static void die(const char *m) {
    printf("ngcd: %s\n", m);
    /* no font on screen yet: a red screen says something went wrong */
    if (gs) {
        for (int i = 0; i < 2; i++) {
            gsKit_clear(gs, GS_SETREG_RGBAQ(0x60, 0x00, 0x00, 0x80, 0x00));
            gsKit_queue_exec(gs);
            gsKit_sync_flip(gs);
        }
    }
    for (;;) SleepThread();
}

static void *load_file(const char *name, uint32_t *size, int required) {
    char p[320];
    snprintf(p, sizeof p, "%s%s", base_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        if (required) { printf("ngcd: missing %s\n", p); die("missing file"); }
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = memalign(64, (size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read error");
    fclose(f);
    if (size) *size = (uint32_t)n;
    return b;
}

/* ------------------------------------------------------------ data track */
#define CACHE_SECTORS 32                     /* 64 KB read-ahead */
static FILE *iso;
static uint32_t iso_sectors;
static uint8_t cache[CACHE_SECTORS * 2048] __attribute__((aligned(64)));
static uint32_t cache_first = 0xFFFFFFFF, cache_count;

static int rd_sector(uint32_t s, uint8_t *buf) {
    if (s >= iso_sectors) { memset(buf, 0, 2048); return 0; }
    if (s < cache_first || s >= cache_first + cache_count) {
        uint32_t n = iso_sectors - s;
        if (n > CACHE_SECTORS) n = CACHE_SECTORS;
        if (fseek(iso, (long)s * 2048, SEEK_SET) != 0 ||
            fread(cache, 2048, n, iso) != n) {
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
/* The track is read in the main loop, a few blocks a frame ahead of the
   mixer (the ring holds 0.85 s; the files are 44 KB/s). */
#define RING_BLOCKS 64
#define READ_BATCH  8
static uint8_t ring[RING_BLOCKS][NGA_BLOCK];
static uint32_t ring_head, ring_tail;
static FILE *trk;
static int trk_play;
static uint32_t trk_sector, trk_sectors;
static int silence_blocks;
static int16_t cd_pcm[NGA_SAMPLES * 2];
static int cd_pos = NGA_SAMPLES;
static uint32_t cd_underruns;

static void au_stop(void) {
    if (trk) { fclose(trk); trk = NULL; }
    trk_play = 0;
    ring_head = ring_tail = 0;
    silence_blocks = 0;
    cd_pos = NGA_SAMPLES;
}

static void au_play(int track, int32_t off) {
    au_stop();
    silence_blocks = off < 0 ? -off : 0;
    char name[32];
    snprintf(name, sizeof name, "TRK%02d.NGA", track);
    char p[320];
    snprintf(p, sizeof p, "%s%s", base_dir, name);
    trk = fopen(p, "rb");
    uint8_t h[NGA_HEADER];
    if (!trk || fread(h, 1, NGA_HEADER, trk) != NGA_HEADER || memcmp(h, "NGA1", 4)) {
        printf("ngcd: cannot play %s\n", p);
        if (trk) { fclose(trk); trk = NULL; }
        return;
    }
    trk_sectors = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
    trk_sector = off < 0 ? 0 : (uint32_t)off;
    if (trk_sector >= trk_sectors ||
        fseek(trk, NGA_HEADER + (long)trk_sector * NGA_BLOCK, SEEK_SET) != 0) {
        fclose(trk); trk = NULL;
        return;
    }
    trk_play = 1;
}

static void cd_fill(void) {
    static uint8_t tmp[READ_BATCH * NGA_BLOCK];
    if (!trk_play || RING_BLOCKS - (ring_head - ring_tail) < READ_BATCH) return;
    uint32_t n = trk_sectors - trk_sector;
    if (n > READ_BATCH) n = READ_BATCH;
    if (n == 0 || fread(tmp, NGA_BLOCK, n, trk) != n) { trk_play = 0; return; }
    trk_sector += n;
    for (uint32_t i = 0; i < n; i++)
        memcpy(ring[(ring_head + i) % RING_BLOCKS], tmp + i * NGA_BLOCK, NGA_BLOCK);
    ring_head += n;
}

static inline void cd_next(int *l, int *r) {
    if (cd_pos >= NGA_SAMPLES) {
        if (silence_blocks > 0) {
            silence_blocks--;
            memset(cd_pcm, 0, sizeof cd_pcm);
        } else if (ring_head != ring_tail) {
            nga_decode_block(ring[ring_tail % RING_BLOCKS], cd_pcm);
            ring_tail++;
        } else {
            if (trk_play) cd_underruns++;
            *l = *r = 0;
            return;
        }
        cd_pos = 0;
    }
    *l = cd_pcm[2 * cd_pos];
    *r = cd_pcm[2 * cd_pos + 1];
    cd_pos++;
}

static const ngcd_disc_io_t disc_io = { rd_sector, au_play, au_stop };

/* ------------------------------------------------------------ backup RAM */
#define MC_DIR  "mc0:/NGCDPS2"
#define MC_FILE MC_DIR "/BACKUP.BIN"
static uint8_t saved_backup[NGCD_BACKUP_SIZE];
static uint32_t backup_check_frames;
static int backup_dirty_checks;

static void backup_load(void) {
    FILE *f = fopen(MC_FILE, "rb");
    if (f) {
        if (fread(ng.backup, 1, NGCD_BACKUP_SIZE, f) == NGCD_BACKUP_SIZE)
            printf("ngcd: backup RAM loaded from the memory card\n");
        fclose(f);
    }
    memcpy(saved_backup, ng.backup, NGCD_BACKUP_SIZE);
}

/* once a second: write when the backup RAM changed and then stayed the same
   (a memory card write takes a while; it is done in the main loop) */
static void backup_poll(void) {
    if (++backup_check_frames < 60) return;
    backup_check_frames = 0;
    static uint8_t last[NGCD_BACKUP_SIZE];
    int changed = memcmp(ng.backup, saved_backup, NGCD_BACKUP_SIZE) != 0;
    int stable = memcmp(ng.backup, last, NGCD_BACKUP_SIZE) == 0;
    memcpy(last, ng.backup, NGCD_BACKUP_SIZE);
    if (!changed || !stable) { backup_dirty_checks = 0; return; }
    if (++backup_dirty_checks < 2) return;
    backup_dirty_checks = 0;
    mkdir(MC_DIR, 0777);
    FILE *f = fopen(MC_FILE, "wb");
    if (f) {
        if (fwrite(ng.backup, 1, NGCD_BACKUP_SIZE, f) == NGCD_BACKUP_SIZE)
            memcpy(saved_backup, ng.backup, NGCD_BACKUP_SIZE);
        fclose(f);
        printf("ngcd: backup RAM saved\n");
    }
}

/* ------------------------------------------------------------ settings */
/* NGCD.CFG, lines "key=value":
     idle=0|1             skip the 68000's wait loops (default 1)
     cpu=12|15|18|21|24   68000 clock in MHz (12 = real hardware)
     region=jp|us|eu      the console's region (default jp)
     video=240p|480i      240p: 320x224 one to one on a TV (default);
                          480i: 640x448 interlaced, every pixel 2x2
     tv=ntsc|pal          output (default ntsc: the games are 60 Hz;
                          pal runs them at 50 frames a second)
     filter=0|1           bilinear filter (default 0: sharp pixels)
     stick=0|1            the left analog stick as the joystick (default 1) */
static int cfg_region = 0, cfg_480i = 0, cfg_pal = 0, cfg_filter = 0, cfg_stick = 1;

static int cfg_is(const char *v, const char *a, const char *b, const char *c) {
    size_t n = strcspn(v, " \t\r");
    return (n == strlen(a) && !strncasecmp(v, a, n)) || (b && n == strlen(b) && !strncasecmp(v, b, n))
        || (c && n == strlen(c) && !strncasecmp(v, c, n));
}

static void load_config(void) {
    uint32_t n;
    char *buf = load_file("NGCD.CFG", &n, 0);
    if (!buf) return;
    char *t = malloc(n + 1);
    memcpy(t, buf, n); t[n] = 0;
    free(buf);
    for (char *l = t; l && *l; ) {
        char *next = strchr(l, '\n');
        if (next) *next++ = 0;
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "idle=", 5)) ngcd_idle_skip = atoi(l + 5) != 0;
        if (!strncmp(l, "cpu=", 4)) {
            int mhz = atoi(l + 4);
            if (mhz >= 12 && mhz <= 24) ngcd_cpu_clock8 = mhz / 3;
        }
        if (!strncmp(l, "region=", 7)) {
            const char *v = l + 7;
            if (cfg_is(v, "jp", "japan", "0")) cfg_region = 0;
            else if (cfg_is(v, "us", "usa", "1")) cfg_region = 1;
            else if (cfg_is(v, "eu", "europe", "2")) cfg_region = 2;
        }
        if (!strncmp(l, "video=", 6)) cfg_480i = cfg_is(l + 6, "480i", "480", "640");
        if (!strncmp(l, "tv=", 3)) cfg_pal = cfg_is(l + 3, "pal", "50", NULL);
        if (!strncmp(l, "filter=", 7)) cfg_filter = atoi(l + 7) != 0;
        if (!strncmp(l, "stick=", 6)) cfg_stick = atoi(l + 6) != 0;
        l = next;
    }
    free(t);
    printf("ngcd: 68000 %d MHz, idle %d, region %d, %s, %s\n", ngcd_cpu_clock8 * 3, ngcd_idle_skip,
           cfg_region, cfg_480i ? "480i" : "240p", cfg_pal ? "PAL" : "NTSC");
}

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

/* ------------------------------------------------------------ pads */
static uint8_t pad_area[2][256] __attribute__((aligned(64)));
static int pad_open[2];
static uint8_t pad[2] = { 0xFF, 0xFF }, sys = 0x0F;
static int quit_combo;

static void pads_init(void) {
    for (int p = 0; p < 2; p++) pad_open[p] = padPortOpen(p, 0, pad_area[p]) != 0;
}

static void pads_read(void) {
    for (int p = 0; p < 2; p++) {
        uint16_t b = 0;
        int jx = 0, jy = 0;
        if (pad_open[p]) {
            int st = padGetState(p, 0);
            if (st == PAD_STATE_STABLE || st == PAD_STATE_FINDCTP1) {
                static int analog_set[2];
                if (!analog_set[p]) { padSetMainMode(p, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_UNLOCK); analog_set[p] = 1; }
                struct padButtonStatus s;
                if (padRead(p, 0, &s)) {
                    b = (uint16_t)(0xFFFF ^ s.btns);
                    if ((s.mode >> 4) == 0x7) { jx = s.ljoy_h - 128; jy = s.ljoy_v - 128; }
                }
            }
        }
        if (cfg_stick) {
            if (jx < -64) b |= PAD_LEFT;
            if (jx > 64)  b |= PAD_RIGHT;
            if (jy < -64) b |= PAD_UP;
            if (jy > 64)  b |= PAD_DOWN;
        }
        if ((b & PAD_LEFT) && (b & PAD_RIGHT)) b &= (uint16_t)~(PAD_LEFT | PAD_RIGHT);
        if ((b & PAD_UP) && (b & PAD_DOWN)) b &= (uint16_t)~(PAD_UP | PAD_DOWN);
        /* Neo Geo pad: U D L R A B C D, active low; cross A, circle B,
           square C, triangle D (as on the Neo Geo CD pad's layout) */
        uint8_t v = 0xFF;
        if (b & PAD_UP)       v &= ~0x01;
        if (b & PAD_DOWN)     v &= ~0x02;
        if (b & PAD_LEFT)     v &= ~0x04;
        if (b & PAD_RIGHT)    v &= ~0x08;
        if (b & PAD_CROSS)    v &= ~0x10;
        if (b & PAD_CIRCLE)   v &= ~0x20;
        if (b & PAD_SQUARE)   v &= ~0x40;
        if (b & PAD_TRIANGLE) v &= ~0x80;
        pad[p] = v;
        uint8_t start = p ? 0x04 : 0x01, select = p ? 0x08 : 0x02;
        if (b & PAD_START)  sys &= ~start;  else sys |= start;
        if (b & PAD_SELECT) sys &= ~select; else sys |= select;
        if (p == 0) quit_combo = (b & (PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2 | PAD_START | PAD_SELECT)) ==
                                 (PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2 | PAD_START | PAD_SELECT);
    }
}

/* ------------------------------------------------------------ sound */
/* YM2610 at 22050 Hz -> 44100 Hz with linear interpolation, + CD audio;
   the resampling step follows the backlog (the machine runs at 59.6 Hz,
   the TV at 59.94). Same as the Dreamcast build. */
static int16_t ym_buf[1024 * 2];
static int ym_have, ym_pos;
static int ym_prev_l, ym_prev_r, ym_cur_l, ym_cur_r;
static uint32_t ym_frac;
static int32_t err_f;
static uint32_t ym_underruns, snd_drops;
static uint32_t ym_step_nominal;

static inline void ym_next(void) {
    ym_prev_l = ym_cur_l; ym_prev_r = ym_cur_r;
    if (ym_pos >= ym_have) {
        ym_have = ngcd_audio_take(ym_buf, 1024);
        ym_pos = 0;
        if (!ym_have) { ym_underruns++; return; }
    }
    ym_cur_l = ym_buf[2 * ym_pos];
    ym_cur_r = ym_buf[2 * ym_pos + 1];
    ym_pos++;
}

static inline int16_t clip(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static void mix(int16_t *out, int frames) {
    int backlog = ngcd_audio_avail() + (ym_have - ym_pos);
    int target = frames / 2 + 256;
    int err = backlog - target;
    if (err > 4096) {
        while (backlog > target) {
            int k = ngcd_audio_take(ym_buf, backlog - target > 1024 ? 1024 : backlog - target);
            if (!k) break;
            backlog -= k;
        }
        ym_have = ym_pos = 0;
        err = 0;
        err_f = 0;
    }
    err_f += (err * 16 - err_f) / 32;
    int32_t adj = err_f / 256;
    if (adj > 66) adj = 66;
    if (adj < -66) adj = -66;
    uint32_t step = (uint32_t)((int32_t)ym_step_nominal + adj);
    for (int i = 0; i < frames; i++) {
        while (ym_frac >= 65536) { ym_next(); ym_frac -= 65536; }
        int f = (int)(ym_frac >> 1);
        int l = ym_prev_l + (((ym_cur_l - ym_prev_l) * f) >> 15);
        int r = ym_prev_r + (((ym_cur_r - ym_prev_r) * f) >> 15);
        ym_frac += step;
        int cl, cr;
        cd_next(&cl, &cr);
        out[2 * i]     = clip(l + cl);
        out[2 * i + 1] = clip(r + cr);
    }
}

#define SND_MAX_FRAMES 1024
static int16_t snd_out[SND_MAX_FRAMES * 2] __attribute__((aligned(64)));
static uint32_t snd_acc;                      /* 16.16 output frames per TV frame */
static uint32_t snd_per_frame;                /* 16.16 */
static int snd_ok;

static void sound_init(void) {
    if (init_audio_driver() != AUDIO_INIT_STATUS_OK) { printf("ngcd: no audio\n"); return; }
    struct audsrv_fmt_t fmt = { .freq = OUT_RATE, .bits = 16, .channels = 2 };
    if (audsrv_set_format(&fmt) < 0) { printf("ngcd: audsrv_set_format failed\n"); return; }
    audsrv_set_volume(MAX_VOLUME);
    snd_ok = 1;
    double tv = cfg_pal ? 50.0 : 59.94;
    snd_per_frame = (uint32_t)(OUT_RATE / tv * 65536.0);
    /* 65536 * 22050/44100 * tv/59.6 */
    ym_step_nominal = (uint32_t)(32768.0 * tv / 59.6);
    memset(snd_out, 0, sizeof snd_out);
    for (int i = 0; i < 3; i++) audsrv_play_audio((char *)snd_out, 735 * 4);   /* a cushion */
}

/* one TV frame of sound; audsrv's ring is kept between 1 and 4 frames
   (it repeats its whole ring if it ever runs empty) */
static void sound_frame(void) {
    if (!snd_ok) return;
    snd_acc += snd_per_frame;
    int n = (int)(snd_acc >> 16);
    snd_acc &= 0xFFFF;
    if (n > SND_MAX_FRAMES) n = SND_MAX_FRAMES;
    mix(snd_out, n);
    int queued = audsrv_queued() / 4;
    if (queued > 4 * 735) { snd_drops++; return; }
    audsrv_play_audio((char *)snd_out, n * 4);
    if (queued < 735) audsrv_play_audio((char *)snd_out, n * 4);   /* keep it from emptying */
}

/* ------------------------------------------------------------ video */
static GSTEXTURE tex;
static volatile uint32_t vsync_count;

static int vsync_handler(int cause) {
    (void)cause;
    vsync_count++;
    ExitHandler();
    return 0;
}

static void video_init(void) {
    gs = gsKit_init_global();
    gs->Mode = cfg_pal ? GS_MODE_PAL : GS_MODE_NTSC;
    if (cfg_480i) {
        gs->Interlace = GS_INTERLACED; gs->Field = GS_FIELD;
        gs->Width = 640; gs->Height = cfg_pal ? 512 : 448;
    } else {
        gs->Interlace = GS_NONINTERLACED; gs->Field = GS_FRAME;
        gs->Width = 640; gs->Height = cfg_pal ? 256 : 224;
    }
    gs->PSM = GS_PSM_CT16;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    gs->Dithering = GS_SETTING_OFF;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_set_clamp(gs, GS_CMODE_CLAMP);
    gsKit_vram_clear(gs);
    gsKit_init_screen(gs);
    gsKit_TexManager_init(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_add_vsync_handler(vsync_handler);

    memset(&tex, 0, sizeof tex);
    tex.Width = NGCD_FB_W;
    tex.Height = NGCD_FB_H;
    tex.PSM = GS_PSM_CT16;
    tex.Filter = cfg_filter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    tex.Mem = memalign(128, gsKit_texture_size_ee(tex.Width, tex.Height, tex.PSM));
    memset(tex.Mem, 0, gsKit_texture_size_ee(tex.Width, tex.Height, tex.PSM));
}

static void video_draw(void) {
    ngcd_render((uint16_t *)tex.Mem, NGCD_FB_W);
    SyncDCache(tex.Mem, (uint8_t *)tex.Mem + NGCD_FB_W * NGCD_FB_H * 2);
    gsKit_TexManager_invalidate(gs, &tex);
    gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_TexManager_bind(gs, &tex);
    /* 320x224 to the full screen width (640 dots: 2 per pixel); 1:1 lines
       in 240p, 2:1 in 480i; centred on PAL screens */
    float h = cfg_480i ? 448.0f : 224.0f;
    float y0 = (float)(gs->Height - (int)h) / 2.0f;
    gsKit_prim_sprite_texture(gs, &tex, 0.0f, y0, 0.0f, 0.0f, 640.0f, y0 + h,
                              (float)NGCD_FB_W, (float)NGCD_FB_H, 0, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    gsKit_queue_exec(gs);
    gsKit_finish();
    gsKit_sync_flip(gs);
    gsKit_TexManager_nextFrame(gs);
}

/* ------------------------------------------------------------ main */
static void reset_iop(void) {
    SifInitRpc(0);
#ifndef NGCD_KEEP_IOP
    while (!SifIopReset(NULL, 0)) {}
#endif
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

static void set_base_dir(int argc, char **argv) {
    strcpy(base_dir, "mass:/NGCD/");
    if (argc > 1 && argv[1][0]) {                 /* a folder given by the launcher */
        snprintf(base_dir, sizeof base_dir, "%s", argv[1]);
        size_t n = strlen(base_dir);
        if (n && base_dir[n - 1] != '/' && base_dir[n - 1] != ':' && n + 1 < sizeof base_dir) strcat(base_dir, "/");
        return;
    }
    if (argc > 0 && argv[0]) {                    /* the ELF's own folder */
        snprintf(base_dir, sizeof base_dir, "%s", argv[0]);
        char *s = strrchr(base_dir, '/');
        if (!s) s = strrchr(base_dir, '\\');
        if (!s) s = strrchr(base_dir, ':');
        if (s) s[1] = 0;
    }
}

int main(int argc, char **argv) {
    set_base_dir(argc, argv);
    reset_iop();
    init_ps2_filesystem_driver();
    waitUntilDeviceIsReady(base_dir);
    init_joystick_driver(true);
    printf("ngcd-ps2: files from %s\n", base_dir);

    load_config();
    video_init();
    pads_init();

    uint32_t sz_bios, sz_toc;
    uint8_t *bios = load_file("NEOCD.BIN", &sz_bios, 1);
    if (sz_bios != 0x80000) die("NEOCD.BIN: not 512 KB");
    uint8_t *lo = load_file("NG_LO.ROM", NULL, 0);
    uint8_t *tocb = load_file("TOC.BIN", &sz_toc, 1);
    parse_toc(tocb, sz_toc);
    char p[320];
    snprintf(p, sizeof p, "%sDATA.ISO", base_dir);
    iso = fopen(p, "rb");
    if (!iso) die("missing DATA.ISO");
    fseek(iso, 0, SEEK_END);
    iso_sectors = (uint32_t)(ftell(iso) / 2048);

    if (ngcd_init(bios, lo, &toc, &disc_io) != 0) die("out of memory");
    ng.nationality = (uint32_t)cfg_region;
    free(bios); free(lo); free(tocb);
    backup_load();
    sound_init();

    /* one machine frame per TV frame: drawing waits for the vertical
       blank (gsKit_sync_flip); when the emulation has fallen behind the
       TV, the picture is skipped (at most 3 in a row) to catch up */
    uint32_t next = vsync_count, skipped = 0, frames = 0, shown = 0;
    for (;;) {
        pads_read();
        if (quit_combo) break;
        ngcd_set_input(pad[0], pad[1], sys);
        ngcd_run_frame();
        cd_fill();
        sound_frame();
        backup_poll();
        frames++;
        next++;
        int behind = (int32_t)(vsync_count - next);
        if (behind > 8) { next = vsync_count; behind = 0; }   /* a long stall (loading): resync */
        if (behind > 0 && skipped < 3) {
            skipped++;
        } else {
            skipped = 0;
            video_draw();
            shown++;
        }
#ifdef NGCD_PERF
        if (frames % 300 == 0) {
            printf("ngcd: %u frames, %u shown, cdda underruns %u, ym underruns %u, sound drops %u\n",
                   (unsigned)frames, (unsigned)shown, (unsigned)cd_underruns, (unsigned)ym_underruns, (unsigned)snd_drops);
            shown = 0;
        }
#endif
    }
    if (snd_ok) audsrv_stop_audio();
    return 0;
}

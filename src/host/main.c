/* Host test harness for the Neo Geo CD core: runs the real BIOS and a
   converted disc (tools/ngcdconv), dumps frames (PPM) and audio (WAV).
   usage: ngcd_host bios.bin lo.rom|- discdir frames every outdir [script]
   script: "frame:KEYS,frame:KEYS" keys U D L R a b c d S(start) s(select),
           each held for 8 frames */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/ngcd.h"
#include "../core/nga.h"

static FILE *iso, *nga;
static int nga_track;
static uint32_t nga_sector, nga_sectors;
static int16_t nga_buf[NGA_SAMPLES * 2];
static int nga_pos = NGA_SAMPLES;
static const char *discdir;
static int verbose;

static int rd_sector(uint32_t s, uint8_t *buf) {
    if (fseek(iso, (long)s * 2048, SEEK_SET)) return 0;
    return fread(buf, 1, 2048, iso) == 2048;
}

static void au_play(int track, int32_t off) {
    char p[1024];
    if (nga) fclose(nga);
    snprintf(p, sizeof p, "%s/TRK%02d.NGA", discdir, track);
    nga = fopen(p, "rb");
    nga_track = track;
    uint8_t h[NGA_HEADER];
    if (!nga || fread(h, 1, NGA_HEADER, nga) != NGA_HEADER) { nga = NULL; return; }
    nga_sectors = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
    nga_sector = off < 0 ? 0 : (uint32_t)off;
    fseek(nga, NGA_HEADER + (long)nga_sector * NGA_BLOCK, SEEK_SET);
    nga_pos = NGA_SAMPLES;
    if (verbose) fprintf(stderr, "[cdda] play track %d from sector %d\n", track, off);
}

static void au_stop(void) {
    if (nga) { fclose(nga); nga = NULL; if (verbose) fprintf(stderr, "[cdda] stop (track %d)\n", nga_track); }
}

static int cd_sample(int16_t *lr) {
    if (!nga) { lr[0] = lr[1] = 0; return 0; }
    if (nga_pos >= NGA_SAMPLES) {
        uint8_t blk[NGA_BLOCK];
        if (nga_sector >= nga_sectors || fread(blk, 1, NGA_BLOCK, nga) != NGA_BLOCK) { lr[0] = lr[1] = 0; return 0; }
        nga_decode_block(blk, nga_buf);
        nga_sector++;
        nga_pos = 0;
    }
    lr[0] = nga_buf[2 * nga_pos]; lr[1] = nga_buf[2 * nga_pos + 1];
    nga_pos++;
    return 1;
}

static const ngcd_disc_io_t io = { rd_sector, au_play, au_stop };

static void *load(const char *p, long *sz) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*sz);
    if (fread(b, 1, *sz, f) != (size_t)*sz) exit(1);
    fclose(f);
    return b;
}

static int load_toc(const char *dir, ngcd_toc_t *t) {
    char p[1024];
    long sz;
    snprintf(p, sizeof p, "%s/TOC.BIN", dir);
    uint8_t *b = load(p, &sz);
    if (memcmp(b, "NGTOC1", 6)) return -1;
    t->n = b[8] | (b[9] << 8);
    t->total = b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t)b[15] << 24);
    t->first = b[16]; t->last = b[17];
    for (int i = 0; i < t->n; i++) {
        uint8_t *e = b + 20 + i * 12;
        t->e[i].track = e[0]; t->e[i].index = e[1]; t->e[i].is_data = e[2];
        t->e[i].start = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
        t->e[i].length = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
    }
    free(b);
    return 0;
}

static uint16_t fb[NGCD_FB_W * NGCD_FB_H];

static void dump(const char *path) {
    FILE *o = fopen(path, "wb");
    fprintf(o, "P6\n%d %d\n255\n", NGCD_FB_W, NGCD_FB_H);
    for (int i = 0; i < NGCD_FB_W * NGCD_FB_H; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) << 3, o); fputc(((c >> 5) & 63) << 2, o); fputc((c & 31) << 3, o);
    }
    fclose(o);
}

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); uint32_t v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = 44100; fwrite(&v, 4, 1, f); v = 44100 * 4; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: ngcd_host bios lo|- discdir frames every outdir [script]\n"); return 1; }
    long sz;
    uint8_t *bios = load(argv[1], &sz);
    uint8_t *lo = strcmp(argv[2], "-") ? load(argv[2], &sz) : NULL;
    discdir = argv[3];
    verbose = getenv("NGCD_VERBOSE") != NULL;
    static ngcd_toc_t toc;
    if (load_toc(discdir, &toc)) { fprintf(stderr, "bad TOC\n"); return 1; }
    char p[1024];
    snprintf(p, sizeof p, "%s/DATA.ISO", discdir);
    iso = fopen(p, "rb");
    if (!iso) { fprintf(stderr, "no DATA.ISO\n"); return 1; }
    if (ngcd_init(bios, lo, &toc, &io)) { fprintf(stderr, "init failed\n"); return 1; }

    if (getenv("NGCD_CDSPEED")) ng.cd_speed_mul = atoi(getenv("NGCD_CDSPEED"));
    int frames = atoi(argv[4]), every = atoi(argv[5]);
    const char *script = argc > 7 ? argv[7] : NULL;
    snprintf(p, sizeof p, "%s/audio.wav", argv[6]);
    FILE *wav = fopen(p, "wb");
    wav_header(wav, 0);
    uint32_t wav_frames = 0;
    static int16_t ym[4096 * 2];

    for (int f = 0; f < frames; f++) {
        uint8_t p1 = 0xFF, sys = 0x0F;
        for (const char *s = script; s && *s; ) {
            int at = atoi(s);
            const char *c = strchr(s, ':');
            if (!c) break;
            if (f >= at && f < at + 8)
                for (const char *k = c + 1; *k && *k != ','; k++)
                    switch (*k) {
                    case 'U': p1 &= ~0x01; break; case 'D': p1 &= ~0x02; break;
                    case 'L': p1 &= ~0x04; break; case 'R': p1 &= ~0x08; break;
                    case 'a': p1 &= ~0x10; break; case 'b': p1 &= ~0x20; break;
                    case 'c': p1 &= ~0x40; break; case 'd': p1 &= ~0x80; break;
                    case 'S': sys &= ~0x01; break; case 's': sys &= ~0x02; break;
                    }
            s = strchr(c, ',');
            if (s) s++;
        }
        ngcd_set_input(p1, 0xFF, sys);
#ifdef NGCD_DEBUG
        { extern int ngcd_fine; static int ff = -1; if (ff < 0) ff = getenv("NGCD_FINE") ? atoi(getenv("NGCD_FINE")) : 0;
          ngcd_fine = (ff && f >= ff); }
#endif
        ngcd_run_frame();

#ifdef CSIM
        { extern void ngcd_pvr_draw(float, float); extern int ngcd_pvr_init(void); extern void csim_enable(int);
          static int init; if (!init) { ngcd_pvr_init(); init = 1; }
          { static int sk = -1; if (sk < 0) sk = getenv("CSIM_SKIP") ? atoi(getenv("CSIM_SKIP")) : 1; if (f % sk == 0 || (sk > 100 && (rand() % 4))) { csim_enable(1); ngcd_pvr_draw(0, 8); } } }
#endif
        if (getenv("NGCD_QUADSTAT")) {   /* PVR path statistics */
            static ngcd_quad_t q[20000];
            int nq = ngcd_build_quads(q, 20000);
            if (nq > 3000) fprintf(stderr, "frame %d: %d quads\n", f, nq);
        }
        int n = ngcd_audio_take(ym, 4096);
        for (int i = 0; i < n; i++) {
            int16_t cd[2];
            cd_sample(cd);
            int l = ym[2 * i] + cd[0], r = ym[2 * i + 1] + cd[1];
            int16_t o[2] = { (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l),
                             (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r) };
            fwrite(o, 2, 2, wav);
        }
        wav_frames += (uint32_t)n;

        { static int df = -2; if (df == -2) df = getenv("NGCD_DUMPSTATE") ? atoi(getenv("NGCD_DUMPSTATE")) : -1;
          if (f == df) {
            snprintf(p, sizeof p, "%s/state.bin", argv[6]);
            FILE *o = fopen(p, "wb");
            fwrite(ng.vram, 2, NGCD_VRAM_WORDS, o); fwrite(ng.palram, 2, 8192, o);
            fwrite(ng.spr, 4, NGCD_SPR_SIZE / 4, o); fwrite(ng.fix, 1, NGCD_FIX_SIZE, o);
            uint32_t regs[12] = { ng.pal_bank, ng.aa_counter, ng.aa_disabled, ng.spr_disable, ng.fix_disable, ng.video_enable, ng.shadow };
            fwrite(regs, 4, 12, o); fwrite(ng.yzoom, 1, sizeof ng.yzoom, o);
            fwrite(ng.ram, 1, NGCD_RAM_SIZE, o);
            fclose(o);
          } }
        if (every && f % every == every - 1) {
            ngcd_render(fb, NGCD_FB_W);
            snprintf(p, sizeof p, "%s/f%06d.ppm", argv[6], f + 1);
            dump(p);
        }
#ifdef NGCD_DEBUG
        { extern void ngcd_debug_dump(void); uint32_t pc = cpu_pc();
          if (pc >= 0x200000 && pc < 0xC00000) { fprintf(stderr, "frame %d: PC %06X\n", f, pc); ngcd_debug_dump(); return 1; } }
#endif
        if (verbose && f % 60 == 59)
            fprintf(stderr, "f%d pc=%06X sectors=%u cdpos=%u play=%d irqm1=%03X irqm2=%03X z80=%s\n", f + 1,
                    cpu_pc(), ng.sectors_read, ng.cd_pos, ng.cd_playing, ng.irq_mask1, ng.irq_mask2,
                    ng.z80_disable ? "off" : "on");
    }
#ifdef CSIM
    { extern void csim_report(void); csim_report(); }
#endif
    wav_header(wav, wav_frames);
    fclose(wav);
    return 0;
}

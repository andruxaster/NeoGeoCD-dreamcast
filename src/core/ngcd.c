/* Neo Geo CD machine: timers, interrupts, frame loop, sound CPU glue.
   Timings and interrupt wiring after neocd_libretro (neogeocd.cpp,
   timergroup.cpp, z80intf.cpp, audio.cpp). */
#include <stdlib.h>
#include <string.h>
#ifdef NGCD_DEBUG
#include <stdio.h>
#endif
#include "ngcd.h"
#include "ym2610.h"

ngcd_t ng;

#define FRAME_PIXELS (NGCD_SCREEN_W_PX * NGCD_SCREEN_H_PX)
#define VBL_IRQ_X    (NGCD_ACTIVE_LEFT / 2)
#define VBL_IRQ_Y    NGCD_ACTIVE_BOTTOM
#define VBL_RELOAD_X (NGCD_ACTIVE_LEFT + 320 - 63)
#define VBL_RELOAD_Y NGCD_ACTIVE_BOTTOM

/* ------------------------------------------------------------ audio */
#ifndef NGCD_AUDIO_RATE
#define NGCD_AUDIO_RATE 44100
#endif
#define AUDIO_RATE   NGCD_AUDIO_RATE
#define AUDIO_RING   8192          /* stereo frames */
static int16_t  aring[AUDIO_RING * 2];
static uint32_t aring_w, aring_r;
static uint64_t ym_generated;      /* samples since reset */

uint64_t ngcd_z80_time;

#ifdef NGCD_PROF
/* host-provided nanosecond clock; totals are read and reset by the host */
uint64_t ngcd_prof_clock(void);
uint64_t ngcd_prof_ym, ngcd_prof_z80;
static int prof_in_z80;
#define PROF_BEGIN(v) uint64_t v = ngcd_prof_clock()
#define PROF_END(v, acc) (acc) += ngcd_prof_clock() - (v)
#else
#define PROF_BEGIN(v)
#define PROF_END(v, acc)
#endif            /* master time of the Z80 */

uint64_t ngcd_ym_time_now(void) {
    return ngcd_z80_time + (uint64_t)Z802M(z80if_elapsed());
}

static void ym_generate_to(uint64_t t) {
    uint64_t due = t * AUDIO_RATE / NGCD_MASTER_CLOCK;
    while (ym_generated < due) {
        uint32_t n = (uint32_t)(due - ym_generated);
        uint32_t room = AUDIO_RING - (aring_w - aring_r);
        if (n > room) {               /* nobody is taking audio: drop the oldest */
            aring_r += n - room;
        }
        uint32_t pos = aring_w % AUDIO_RING;
        if (n > AUDIO_RING - pos) n = AUDIO_RING - pos;
        if (n > 512) n = 512;
        PROF_BEGIN(t0);
        YM2610UpdateTo(&aring[pos * 2], (int)n);
#ifdef NGCD_PROF
        uint64_t dt = ngcd_prof_clock() - t0;
        ngcd_prof_ym += dt;
        if (prof_in_z80) ngcd_prof_z80 -= dt;      /* Z80 time excludes the YM */
#endif
        aring_w += n;
        ym_generated += n;
    }
}

void YM2610UpdateRequest(void) {
    ym_generate_to(ngcd_ym_time_now());
}

int ngcd_audio_avail(void) { return (int)(aring_w - aring_r); }

int ngcd_audio_take(int16_t *dst, int max_frames) {
    int n = 0;
    while (aring_r != aring_w && n < max_frames) {
        uint32_t pos = aring_r % AUDIO_RING;
        dst[n * 2] = aring[pos * 2];
        dst[n * 2 + 1] = aring[pos * 2 + 1];
        aring_r++; n++;
    }
    return n;
}

static void ym_timer_handler(int channel, int count) {
    int t = channel ? T_YMB : T_YMA;
    if (!count) { ng.t[t].active = 0; return; }
    int32_t cyc = (int32_t)(((uint64_t)count * 144 * 3021 + 500) / 1000);
    /* armed relative to the Z80's current time within the slice */
    int32_t base = (int32_t)(ngcd_ym_time_now() - ng.time);
    ngcd_timer_arm(t, cyc + (base > 0 ? base : 0));
}

static void ym_irq_handler(int irq) { z80if_irq(irq); }

/* ------------------------------------------------------------ timers */
void ngcd_timer_arm(int t, int32_t delay) { ng.t[t].delay = delay; ng.t[t].active = 1; }
void ngcd_timer_arm_rel(int t, int32_t delay) { ng.t[t].delay += delay; ng.t[t].active = 1; }

static void t_vbl(void) {
    if ((ng.irq_mask2 & 0x030) == 0x030) {
        ng.pending_irq |= IRQ_VBL;
        ngcd_update_irq();
    }
    ngcd_vbl_video();
    ng.frame_ready = 1;
    ngcd_timer_arm_rel(T_VBL, PIX2M(FRAME_PIXELS));
}

static void t_hbl(void) {
    if ((ng.hirq_control & 0x10) && (ng.irq_mask2 & 0x300) == 0x300) {
        ng.pending_irq |= IRQ_RASTER;
        ngcd_update_irq();
    }
    if ((ng.hirq_control & 0x80) && ng.hirq_register != 0xFFFFFFFF) {
        uint32_t px = ng.hirq_register + 1;
        if (px > 0x1000000) px = 0x1000000;
        ngcd_timer_arm_rel(T_HBL, PIX2M((int32_t)px));
    } else {
        ng.t[T_HBL].active = 0;
    }
}

static void t_vblreload(void) {
    if ((ng.hirq_control & 0x40) && ng.hirq_register != 0xFFFFFFFF) {
        uint32_t px = ng.hirq_register + 1;
        if (px > 0x1000000) px = 0x1000000;
        ngcd_timer_arm(T_HBL, ng.t[T_VBLRELOAD].delay + PIX2M((int32_t)px));
    }
    ngcd_timer_arm_rel(T_VBLRELOAD, PIX2M(FRAME_PIXELS));
}

static void t_audiocmd(void) {
    ng.audio_command = ng.t_user[T_AUDIOCMD];
    ng.t[T_AUDIOCMD].active = 0;
    if (!ng.z80_nmi_disable)
        z80if_nmi();
}

static void fire(int t) {
    switch (t) {
    case T_VBL:       t_vbl(); break;
    case T_HBL:       t_hbl(); break;
    case T_VBLRELOAD: t_vblreload(); break;
    case T_CDROM:     ngcd_cd_tick(); break;
    case T_YMA:       ng.t[T_YMA].active = 0; YM2610TimerOver(0); break;
    case T_YMB:       ng.t[T_YMB].active = 0; YM2610TimerOver(1); break;
    case T_AUDIOCMD:  t_audiocmd(); break;
    }
}

static void timers_advance(int32_t elapsed) {
    for (int t = 0; t < T_COUNT; t++) {
        if (!ng.t[t].active) continue;
        ng.t[t].delay -= elapsed;
        /* a callback may re-arm relatively; loop while it stays expired */
        while (ng.t[t].active && ng.t[t].delay <= 0) {
            int32_t before = ng.t[t].delay;
            fire(t);
            if (ng.t[t].active && ng.t[t].delay == before) break;   /* no progress */
        }
    }
}

static int32_t timeslice(void) {
    int32_t s = NGCD_CYCLES_PER_FRAME;
    for (int t = 0; t < T_COUNT; t++)
        if (ng.t[t].active && ng.t[t].delay < s) s = ng.t[t].delay;
    return s < 1 ? 1 : s;
}

/* ------------------------------------------------------------ interrupts */
void ngcd_update_irq(void) {
    int level = 0, vector = 0;
    if (ng.pending_irq & IRQ_VBL)    { level = 1; vector = 0x68 / 4; }
    if (ng.pending_irq & IRQ_CDDEC)  { level = 2; ng.cd_vector = 0x54; vector = 0x54 / 4; }
    if (ng.pending_irq & IRQ_CDCOMM) { level = 2; ng.cd_vector = 0x58; vector = 0x58 / 4; }
    if (ng.pending_irq & IRQ_RASTER) { level = 3; vector = 0x64 / 4; }
    cpu_set_irq(level, vector);
}

/* ------------------------------------------------------------ timing helpers */
int32_t ngcd_slice_elapsed(void) {
    return ng.in_slice ? CPU2M(cpu_elapsed()) : 0;
}

int ngcd_screen_y(void) {
    int32_t into_frame = NGCD_CYCLES_PER_FRAME - ng.remaining + ngcd_slice_elapsed();
    int32_t px = M2PIX(into_frame);
    return (VBL_IRQ_Y + (VBL_IRQ_X + px) / NGCD_SCREEN_W_PX) % NGCD_SCREEN_H_PX;
}

/* ------------------------------------------------------------ Z80 I/O */
uint8_t ngcd_z80_in(uint8_t port) {
    switch (port) {
    case 0x00: return (uint8_t)ng.audio_command;
    case 0x04: return YM2610Read(0);
    case 0x05: return YM2610Read(1);
    case 0x06: return YM2610Read(2);
    case 0x07: return YM2610Read(3);
    }
    return 0;
}

void ngcd_z80_out(uint8_t port, uint8_t v) {
    switch (port) {
    case 0x00: ng.audio_command = 0; break;
    case 0x04: YM2610Write(0, v); break;
    case 0x05: YM2610Write(1, v); break;
    case 0x06: YM2610Write(2, v); break;
    case 0x07: YM2610Write(3, v); break;
    case 0x08: ng.z80_nmi_disable = 0; break;
    case 0x0C: ng.audio_result = v; break;
    case 0x18: ng.z80_nmi_disable = 1; break;
    }
}

void ngcd_z80_enable(int on) {
    if (!on) { ng.z80_disable = 1; return; }
    ng.z80_disable = 0;
    z80if_reset();
    YM2610Reset();
}

/* ------------------------------------------------------------ BIOS */
static int pattern_at(const uint8_t *rom_be, uint32_t addr, const uint8_t *p, int n) {
    return memcmp(rom_be + (addr - 0xC00000), p, n) == 0;
}

int ngcd_init(const uint8_t *bios, const uint8_t *lo, const ngcd_toc_t *toc,
              const ngcd_disc_io_t *io) {
    memset(&ng, 0, sizeof(ng));
    ng.ram    = malloc(NGCD_RAM_SIZE);
    ng.rom    = malloc(NGCD_ROM_SIZE);
    ng.spr    = malloc(NGCD_SPR_SIZE);
    ng.fix    = malloc(NGCD_FIX_SIZE);
    ng.pcm    = malloc(NGCD_PCM_SIZE);
    ng.z80ram = malloc(NGCD_Z80_SIZE);
    ng.backup = calloc(1, NGCD_BACKUP_SIZE);
    ng.vram   = malloc(NGCD_VRAM_WORDS * 2);
    if (!ng.ram || !ng.rom || !ng.spr || !ng.fix || !ng.pcm || !ng.z80ram || !ng.backup || !ng.vram)
        return -1;

    /* BIOS: bring to big endian in ng.rom first (file may be byte swapped),
       patch, then store as little-endian words */
    uint8_t *be = ng.rom;
    memcpy(be, bios, NGCD_ROM_SIZE);
    if (be[0] == 0x10 && be[1] == 0x00)
        for (int i = 0; i < NGCD_ROM_SIZE; i += 2) { uint8_t t = be[i]; be[i] = be[i + 1]; be[i + 1] = t; }
    static const uint8_t cdz_id[4] = { 0x00, 0xC0, 0xA3, 0xE8 };
    ng.is_cdz = pattern_at(be, 0xC0006C, cdz_id, 4);
    if (ng.is_cdz) {
        /* CD recognition (copy protection) checks, as neocd_libretro bios.cpp */
        static const uint8_t a[2] = { 0x66, 0x10 }, b[2] = { 0x66, 0x74 };
        if (pattern_at(be, 0xC0EB82, a, 2)) { be[0xEB82] = 0x4E; be[0xEB83] = 0x71; }
        if (pattern_at(be, 0xC0D280, b, 2)) { be[0xD280] = 0x4E; be[0xD281] = 0x71; }
    }
    for (int i = 0; i < NGCD_ROM_SIZE; i += 2) { uint8_t t = be[i]; be[i] = be[i + 1]; be[i + 1] = t; }

    /* Y zoom table: 000-lo.lo if given, else generated (neocd_libretro) */
    if (lo) {
        memcpy(ng.yzoom, lo, sizeof(ng.yzoom));
    } else {
        static const uint8_t order[16] = { 8, 0, 12, 4, 10, 2, 14, 6, 9, 1, 13, 5, 11, 3, 15, 7 };
        uint8_t used[256] = { 0 };
        uint8_t *out = ng.yzoom;
        for (int z = 0; z < 16; z++)
            for (int y = 0; y < 16; y++) {
                used[(order[y] << 4) | order[z]] = 1;
                uint8_t *end = out + 256;
                for (int t = 0; t < 256; t++) if (used[t]) *out++ = (uint8_t)t;
                while (out < end) *out++ = 0xFF;
            }
    }

    ng.toc = *toc;
    ng.io = io;
    ng.nationality = 0;      /* Japan */
    ng.cd_speed_mul = 1;

    cpu_init();
    z80if_init();
    YM2610Init(8000000, AUDIO_RATE, ng.pcm, NGCD_PCM_SIZE, ym_timer_handler, ym_irq_handler);
    ngcd_reset();
    return 0;
}

void ngcd_reset(void) {
    ngcd_mem_reset();
    ngcd_video_reset();
    ngcd_cd_reset();
    memset(ng.t, 0, sizeof(ng.t));
    ng.irq_mask1 = ng.irq_mask2 = 0;
    ng.pending_irq = 0;
    ng.cd_vector = 0;
    ng.cd_comm_nreset = 0;
    ng.z80_disable = 1;
    ng.z80_nmi_disable = 1;
    ng.audio_command = ng.audio_result = 0;
    ng.remaining = 0;
    ng.z80_slice = 0;
    ng.time = 0;
    ngcd_z80_time = 0;
    ym_generated = 0;
    aring_r = aring_w = 0;
    ng.input1 = ng.input2 = 0xFF;
    ng.input3 = 0x0F;
    ng.input_sel = 0;

    /* timers, ADJUST_FRAME_BOUNDARY layout: the frame starts at the VBL IRQ */
    ngcd_timer_arm(T_VBL, PIX2M(FRAME_PIXELS));
    ngcd_timer_arm(T_VBLRELOAD, PIX2M((VBL_RELOAD_Y - VBL_IRQ_Y) * NGCD_SCREEN_W_PX + (VBL_RELOAD_X - VBL_IRQ_X)));
    ngcd_timer_arm(T_CDROM, (int32_t)(NGCD_MASTER_CLOCK / 64.64 + 0.5));

    cpu_reset();
    z80if_reset();
    YM2610Reset();
}

void ngcd_set_input(uint8_t p1, uint8_t p2, uint8_t sys) {
    ng.input1 = p1; ng.input2 = p2; ng.input3 = sys & 0x0F;
}

/* ------------------------------------------------------------ frame */
void ngcd_run_frame(void) {
    ng.remaining += NGCD_CYCLES_PER_FRAME;
    ng.frame_ready = 0;
    while (ng.remaining > 0) {
        int32_t slice = timeslice();
        if (slice > ng.remaining) slice = ng.remaining;
        int32_t req = M2CPU(slice);
        if (req < 1) req = 1;
#ifdef NGCD_DEBUG
        extern int ngcd_fine;
        if (ngcd_fine) req = 4;
#endif
        ng.slice_len = slice;
        ng.in_slice = 1;
        int32_t executed = cpu_run(req);
        ng.in_slice = 0;
#ifdef NGCD_DEBUG
        if (ngcd_fine) { extern void ngcd_debug_line(void); ngcd_debug_line(); }
#endif

        int32_t elapsed = CPU2M(executed);
        if (elapsed < 1) elapsed = 1;

        ng.z80_slice += elapsed;
        if (ng.z80_slice > 0) {
            int32_t z80el;
            if (ng.z80_disable) {
                z80el = ng.z80_slice;
            } else {
                int32_t want = M2Z80(ng.z80_slice);
#ifdef NGCD_PROF
                PROF_BEGIN(tz);
                prof_in_z80 = 1;
#endif
                z80el = want > 0 ? Z802M(z80if_run(want)) : 0;
#ifdef NGCD_PROF
                prof_in_z80 = 0;
                PROF_END(tz, ngcd_prof_z80);
#endif
            }
            ng.z80_slice -= z80el;
            ngcd_z80_time += (uint64_t)z80el;
        }
        ng.remaining -= elapsed;
        ng.time += (uint64_t)elapsed;
        timers_advance(elapsed);
    }
    ym_generate_to(ng.time);
}

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
/* written by the emulation, read by the audio thread on the Dreamcast:
   only the producer moves aring_w, only the consumer aring_r */
#define RING_BARRIER() __asm__ __volatile__("" ::: "memory")
static volatile uint32_t aring_w, aring_r;
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
        uint32_t w = aring_w, room = AUDIO_RING - (w - aring_r);
        uint32_t pos = w % AUDIO_RING;
        if (n > AUDIO_RING - pos) n = AUDIO_RING - pos;
        if (n > 512) n = 512;
        if (n > room && room) n = room;
        if (!room) {
            /* nobody is taking audio: the chip still runs, its output is
               dropped (the consumer owns aring_r) */
            static int16_t scratch[512 * 2];
            YM2610UpdateTo(scratch, (int)n);
            ym_generated += n;
            continue;
        }
        PROF_BEGIN(t0);
        YM2610UpdateTo(&aring[pos * 2], (int)n);
#ifdef NGCD_PROF
        uint64_t dt = ngcd_prof_clock() - t0;
        ngcd_prof_ym += dt;
        if (prof_in_z80) ngcd_prof_z80 -= dt;      /* Z80 time excludes the YM */
#endif
        RING_BARRIER();
        aring_w = w + n;
        ym_generated += n;
    }
}

void YM2610UpdateRequest(void) {
    ym_generate_to(ngcd_ym_time_now());
}

int ngcd_audio_avail(void) { return (int)(aring_w - aring_r); }

int ngcd_audio_take(int16_t *dst, int max_frames) {
    uint32_t r = aring_r, w = aring_w;
    RING_BARRIER();
    int n = 0;
    while (r != w && n < max_frames) {
        uint32_t pos = r % AUDIO_RING;
        dst[n * 2] = aring[pos * 2];
        dst[n * 2 + 1] = aring[pos * 2 + 1];
        r++; n++;
    }
    RING_BARRIER();
    aring_r = r;
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
/* armed from inside a 68000 slice (a register write) to fire before the
   slice's planned end: the slice ends here, so the timer fires on time
   (the raster timer reloaded by a write: Neo Turf Masters' lake, else
   the interrupts until the slice end were lost) */
static inline void arm_check(int t) {
    if (ng.in_slice && ng.t[t].delay < ng.slice_len) cpu_end_slice();
}
void ngcd_timer_arm(int t, int32_t delay) { ng.t[t].delay = delay; ng.t[t].active = 1; arm_check(t); }
void ngcd_timer_arm_rel(int t, int32_t delay) { ng.t[t].delay += delay; ng.t[t].active = 1; arm_check(t); }

static void t_vbl(void) {
    if ((ng.irq_mask2 & 0x030) == 0x030) {
        ng.pending_irq |= IRQ_VBL;
        ngcd_update_irq();
    }
    ngcd_vbl_video();
    ng.frame_ready = 1;
    ngcd_timer_arm_rel(T_VBL, PIX2M(FRAME_PIXELS));
}

uint32_t ngcd_hbl_irqs;                       /* raster interrupts during this frame's picture */

static void t_hbl(void) {
    if ((ng.hirq_control & 0x10) && (ng.irq_mask2 & 0x300) == 0x300) {
        int row = ngcd_screen_y() - NGCD_ACTIVE_TOP;
        if (row >= 0 && row < NGCD_FB_H) ngcd_hbl_irqs++;
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

/* ------------------------------------------------------------ wait loops */
/* Games wait for the next frame in a loop such as
       bclr  #7,($808D,a5)   ; Metal Slug 2
       beq.s *-6
   which only an interrupt can end. When the 68000 is found in such a loop
   at the end of a slice and no interrupt it can take is pending, the
   following slices are not executed until the interrupt line changes
   (time, timers, Z80 and CD go on as usual). Only two-instruction loops
   that read RAM and change nothing are taken: TST / BTST / CMPI of memory
   + BEQ.S / BNE.S back to it, and BCLR + BEQ.S (it only clears a bit that
   is already 0 while looping). Loops that count or write (Metal Slug's)
   are left alone. Saves most of the 68000 time spent waiting, which grows
   with an overclocked 68000. */
int ngcd_idle_skip = 1;
uint32_t ngcd_idle_slices;
static int idle_now;
static uint32_t irq_stamp, idle_stamp;
static int irq_level_now;
#ifndef NGCD_IDLE_SLICE_DIV
#define NGCD_IDLE_SLICE_DIV 32
#endif
int32_t idle_slice_max = NGCD_CYCLES_PER_FRAME / NGCD_IDLE_SLICE_DIV;

static inline uint16_t ram_w16(uint32_t a) { return *(const uint16_t *)(ng.ram + (a & 0x1FFFFE)); }

/* length in bytes of an effective address extension for the allowed modes,
   and the address it reads (RAM only), or -1 */
static int ea_ram(int mode, int reg, uint32_t ext_at, uint32_t *addr) {
    if (mode == 5) {                                   /* (d16,An) */
        if (reg == 7) return -1;
        *addr = cpu_areg(reg) + (uint32_t)(int32_t)(int16_t)ram_w16(ext_at);
        return 2;
    }
    if (mode == 7 && reg == 0) { *addr = (uint32_t)(int32_t)(int16_t)ram_w16(ext_at); return 2; }
    if (mode == 7 && reg == 1) { *addr = ((uint32_t)ram_w16(ext_at) << 16) | ram_w16(ext_at + 2); return 4; }
    return -1;
}

/* the test instruction at t: its length, or 0 if not an allowed one;
   *bclr set for BCLR #n */
static int loop_test(uint32_t t, int *bclr) {
    uint16_t op = ram_w16(t);
    int mode = (op >> 3) & 7, reg = op & 7, len, e;
    uint32_t a;
    *bclr = 0;
    if ((op & 0xFFC0) == 0x0800 || (op & 0xFFC0) == 0x0880) {      /* BTST / BCLR #n,<ea> */
        *bclr = (op & 0xFFC0) == 0x0880;
        e = ea_ram(mode, reg, t + 4, &a); len = 4;
    } else if ((op & 0xFF00) == 0x4A00 && (op & 0xC0) != 0xC0) {   /* TST.x <ea> */
        e = ea_ram(mode, reg, t + 2, &a); len = 2;
    } else if ((op & 0xFF00) == 0x0C00 && (op & 0xC0) != 0xC0) {   /* CMPI.x #imm,<ea> */
        int imm = (op & 0xC0) == 0x80 ? 4 : 2;
        e = ea_ram(mode, reg, t + 2 + imm, &a); len = 2 + imm;
    } else {
        return 0;
    }
    if (e < 0 || (a & 0xFFFFFF) >= NGCD_RAM_SIZE) return 0;
    return len + e;
}

/* is pc inside a skippable wait loop */
static int idle_loop_at(uint32_t pc) {
    if (pc >= NGCD_RAM_SIZE - 16 || (pc & 1)) return 0;
    for (int k = 0; k < 2; k++) {
        uint32_t t;
        if (k == 0) {
            t = pc;                                         /* at the test */
        } else {
            uint16_t b = ram_w16(pc);                       /* at the branch */
            if ((b & 0xFE00) != 0x6600 || (b & 0xFF) == 0 || (b & 0xFF) == 0xFF) return 0;
            t = pc + 2 + (uint32_t)(int32_t)(int8_t)(b & 0xFF);
        }
        int bclr, len = loop_test(t, &bclr);
        if (!len) continue;
        uint32_t bpc = t + len;
        uint16_t b = ram_w16(bpc);
        if ((b & 0xFE00) != 0x6600) continue;              /* BNE.S / BEQ.S */
        if (bpc + 2 + (uint32_t)(int32_t)(int8_t)(b & 0xFF) != t) continue;
        if (bclr && (b & 0xFF00) != 0x6700) continue;      /* BCLR only with BEQ */
        if (pc != t && pc != bpc) continue;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ interrupts */
void ngcd_update_irq(void) {
    int level = 0, vector = 0;
    if (ng.pending_irq & IRQ_VBL)    { level = 1; vector = 0x68 / 4; }
    if (ng.pending_irq & IRQ_CDDEC)  { level = 2; ng.cd_vector = 0x54; vector = 0x54 / 4; }
    if (ng.pending_irq & IRQ_CDCOMM) { level = 2; ng.cd_vector = 0x58; vector = 0x58 / 4; }
    if (ng.pending_irq & IRQ_RASTER) { level = 3; vector = 0x64 / 4; }
    if (level != irq_level_now) { irq_level_now = level; irq_stamp++; }
    cpu_set_irq(level, vector);
}

/* ------------------------------------------------------------ timing helpers */
int32_t ngcd_slice_elapsed(void) {
    return ng.in_slice ? CPU2M(cpu_elapsed()) : 0;
}

int ngcd_wtrace;
int ngcd_screen_x(void) {
    int32_t into_frame = NGCD_CYCLES_PER_FRAME - ng.remaining + ngcd_slice_elapsed();
    return (VBL_IRQ_X + M2PIX(into_frame)) % NGCD_SCREEN_W_PX;
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

#ifndef NGCD_CPU_MHZ
#define NGCD_CPU_MHZ 12
#endif
int ngcd_cpu_clock8 = NGCD_CPU_MHZ / 3;

/* ------------------------------------------------------------ frame */
void ngcd_run_frame(void) {
    ng.remaining += NGCD_CYCLES_PER_FRAME;
    ng.frame_ready = 0;
    ngcd_late_reset();
    while (ng.remaining > 0) {
        int32_t slice = timeslice();
        if (slice > ng.remaining) slice = ng.remaining;
        /* a wait loop is only noticed at the end of a slice: keep running
           slices short enough not to spin in it for long */
        if (ngcd_idle_skip && !idle_now && slice > idle_slice_max) slice = idle_slice_max;
        int32_t req = M2CPU(slice);
        if (req < 1) req = 1;
#ifdef NGCD_DEBUG
        extern int ngcd_fine;
        if (ngcd_fine) req = 4;
#endif
        ng.slice_len = slice;
        int32_t executed;
        if (idle_now && irq_stamp == idle_stamp) {
            executed = req;                    /* waiting for an interrupt */
            ngcd_idle_slices++;
        } else {
            idle_now = 0;
            ng.in_slice = 1;
            executed = cpu_run(req);
            ng.in_slice = 0;
            if (ngcd_idle_skip && executed >= req && irq_level_now <= cpu_irq_mask() &&
                idle_loop_at(cpu_pc())) {
                idle_now = 1;
                idle_stamp = irq_stamp;
            }
        }
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

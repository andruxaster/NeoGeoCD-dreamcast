/* 68000 glue: FAME C (PicoDrive's cpu/fame, C version). RAM and ROM are
   little-endian words (the core's layout), which FAME C fetches from
   directly; data accesses go through the callbacks below, with RAM done
   inline. Interrupts: the Neo Geo keeps a level up until the program
   acknowledges it and gives its own vectors (VBL 0x68, CD 0x54/0x58,
   raster 0x64), so the level stays in interrupts[0] (an iack handler that
   does nothing) and the vector goes to famec_irq_vector. */
#include <stdint.h>
#include <string.h>
#include "ngcd.h"
#include "fame.h"

extern int famec_irq_vector;
static M68K_CONTEXT ctx;
static int running;
static int32_t run_req, run_cut;
/* a stray jump outside RAM/ROM lands in "bra *" instead of host memory */
static uint16_t trap_page[0x8000] __attribute__((aligned(4)));

#define RD8(p, a)   ((p)[(a) ^ 1])
#define RD16(p, a)  (*(uint16_t *)((p) + (a)))
#define WR8(p, a, v)  ((p)[(a) ^ 1] = (uint8_t)(v))
#define WR16(p, a, v) (*(uint16_t *)((p) + (a)) = (uint16_t)(v))

static unsigned r8(unsigned a) {
    a &= 0xFFFFFF;
    if (a < 0x200000) return RD8(ng.ram, a);
    return ngcd_read8(a);
}
static unsigned r16(unsigned a) {
    a &= 0xFFFFFE;
    if (a < 0x200000) return RD16(ng.ram, a);
    return ngcd_read16(a);
}
static unsigned r32(unsigned a) {
    a &= 0xFFFFFE;
    if (a < 0x1FFFFE) return (unsigned)RD16(ng.ram, a) << 16 | RD16(ng.ram, a + 2);
    return (unsigned)ngcd_read16(a) << 16 | ngcd_read16((a + 2) & 0xFFFFFF);
}
static void w8(unsigned a, unsigned char v) {
    a &= 0xFFFFFF;
    if (a >= 0x1000 && a < 0x200000) { WR8(ng.ram, a, v); return; }
    ngcd_write8(a, v);
}
static void w16(unsigned a, unsigned short v) {
    a &= 0xFFFFFE;
    if (a >= 0x1000 && a < 0x200000) { WR16(ng.ram, a, v); return; }
    ngcd_write16(a, v);
}
static void w32(unsigned a, unsigned v) {
    w16(a, (unsigned short)(v >> 16));
    w16(a + 2, (unsigned short)v);
}
static void iack(unsigned level) { (void)level; }   /* the level stays up */

static void map_fetch(unsigned lo, unsigned hi, void *base) {
    for (unsigned i = lo >> 16; i <= hi >> 16; i++)
        ctx.Fetch[i] = (uintptr_t)base - (lo & 0xFF0000);
}

void cpu_init(void) {
    for (int i = 0; i < 0x8000; i++) trap_page[i] = 0x60FE;
    memset(&ctx, 0, sizeof(ctx));
    for (unsigned i = 0; i < M68K_FETCHBANK1; i++) ctx.Fetch[i] = (uintptr_t)trap_page - (i << 16);
    map_fetch(0x000000, 0x1FFFFF, ng.ram);
    map_fetch(0xC00000, 0xC7FFFF, ng.rom);
    map_fetch(0xC80000, 0xCFFFFF, ng.rom);
    ctx.read_byte = r8; ctx.read_word = r16; ctx.read_long = r32;
    ctx.write_byte = w8; ctx.write_word = w16; ctx.write_long = w32;
    ctx.iack_handler = iack;
    fm68k_init();
}

void cpu_reset(void) {
    famec_irq_vector = 0;
    ctx.interrupts[0] = 0;
    ctx.execinfo = 0;
    fm68k_reset(&ctx);
}

int32_t cpu_run(int32_t cycles) {
    run_req = cycles; run_cut = 0; running = 1;
    int32_t done = fm68k_emulate(&ctx, cycles, fm68k_reason_emulate);
    running = 0;
    return done - run_cut;
}

int32_t cpu_elapsed(void) {
    return running ? run_req - (ctx.io_cycle_counter + ctx.cycles_needed) - run_cut : 0;
}

/* stop after the current instruction; the cycles not run are taken back */
void cpu_end_slice(void) {
    if (!running) return;
    int32_t left = ctx.io_cycle_counter + ctx.cycles_needed;
    if (left <= 0) return;
    run_cut += left;
    ctx.io_cycle_counter = 0;
    ctx.cycles_needed = 0;
}

void cpu_set_irq(int level, int vector) {
    if (ctx.interrupts[0] == level && (level == 0 || famec_irq_vector == vector)) return;
    ctx.interrupts[0] = (unsigned char)level;
    famec_irq_vector = level ? vector : 0;
    if (running && level) cpu_end_slice();       /* taken at the next run's start */
}

uint32_t cpu_pc(void) { return fm68k_get_pc(&ctx) & 0xFFFFFF; }
uint32_t cpu_areg(int n) { return ctx.areg[n & 7].D; }
int cpu_irq_mask(void) { return (ctx.sr >> 8) & 7; }

/* Dreamcast 68000 glue: FAME SH4 (fame.s). RAM and BIOS are kept as
   little-endian words, FAME's native layout. Reads of the vector area go
   straight to RAM (it holds the current view); writes there go through
   ngcd_write*, which drops them while the ROM vectors are mapped. */
#include <stdint.h>
#include <string.h>
#include "../core/ngcd.h"
#include "../../third_party/fame/fame_api.h"

unsigned m68k_get_cycles_counter(void) FAME_SYM(m68k_get_cycles_counter);
unsigned m68k_stop_emulating(void)     FAME_SYM(m68k_stop_emulating);

static struct M68K_PROGRAM prg[5];
/* a stray jump outside RAM/ROM lands in "bra *" instead of host memory */
static uint16_t trap_page[0x8000] __attribute__((aligned(4)));   /* 64 KB */
static struct M68K_DATA rd8[5], rd16[5], wr8[4], wr16[4];
/* FAME resolves addresses through 4 KB pages filled from these lists in
   order (later entries win), and host = data + address, with data set so
   that the region's first address lands on 'base'. */
static struct M68K_CONTEXT ctx;
static unsigned run_start;
static int running, cur_level;
static int want_level, want_vector, want_dirty;
static void apply_irq(void);

static unsigned io_r8(unsigned a)  { return ngcd_read8(a); }
static unsigned io_r16(unsigned a) { return ngcd_read16(a); }
static void io_w8(unsigned a, unsigned v)  { ngcd_write8(a, (uint8_t)v); }
static void io_w16(unsigned a, unsigned v) { ngcd_write16(a, (uint16_t)v); }

static void set_data(struct M68K_DATA *d, unsigned lo, unsigned hi, uint8_t *base) {
    d->low_addr = lo; d->high_addr = hi; d->mem_handler = NULL;
    d->data = (void *)((uintptr_t)base - lo);
}
static void set_handler(struct M68K_DATA *d, unsigned lo, unsigned hi, void *h) {
    d->low_addr = lo; d->high_addr = hi; d->mem_handler = h; d->data = NULL;
}
static void set_end(struct M68K_DATA *d) {
    d->low_addr = d->high_addr = (unsigned)-1; d->mem_handler = NULL; d->data = NULL;
}
static void set_prg(struct M68K_PROGRAM *p, unsigned lo, unsigned hi, uint8_t *base) {
    p->low_addr = lo; p->high_addr = hi; p->offset = (unsigned)((uintptr_t)base - lo);
}

static void build_read(struct M68K_DATA *t, void *h) {
    set_handler(&t[0], 0x200000, 0xFFFFFF, h);          /* first: ROM below wins */
    set_data(&t[1], 0x000000, 0x1FFFFF, ng.ram);
    set_data(&t[2], 0xC00000, 0xC7FFFF, ng.rom);
    set_data(&t[3], 0xC80000, 0xCFFFFF, ng.rom);
    set_end(&t[4]);
}
static void build_write(struct M68K_DATA *t, void *h) {
    set_handler(&t[0], 0x000000, 0x000FFF, h);          /* vector page: mapping rules */
    set_data(&t[1], 0x001000, 0x1FFFFF, ng.ram + 0x1000);
    set_handler(&t[2], 0x200000, 0xFFFFFF, h);
    set_end(&t[3]);
}

void cpu_init(void) {
    set_prg(&prg[0], 0x000000, 0x1FFFFF, ng.ram);
    set_prg(&prg[1], 0xC00000, 0xC7FFFF, ng.rom);
    set_prg(&prg[2], 0xC80000, 0xCFFFFF, ng.rom);
    for (int i = 0; i < 0x8000; i++) trap_page[i] = 0x60FE;
    prg[3].low_addr = 0x400000; prg[3].high_addr = 0x40FFFF;
    prg[3].offset = (unsigned)((uintptr_t)trap_page - 0x400000);
    prg[4].low_addr = prg[4].high_addr = (unsigned)-1; prg[4].offset = 0;
    build_read(rd8, (void *)io_r8);
    build_read(rd16, (void *)io_r16);
    build_write(wr8, (void *)io_w8);
    build_write(wr16, (void *)io_w16);
    m68k_init();
    memset(&ctx, 0, sizeof(ctx));
    ctx.fetch = ctx.sv_fetch = ctx.user_fetch = prg;
    ctx.read_byte  = ctx.sv_read_byte  = ctx.user_read_byte  = rd8;
    ctx.read_word  = ctx.sv_read_word  = ctx.user_read_word  = rd16;
    ctx.write_byte = ctx.sv_write_byte = ctx.user_write_byte = wr8;
    ctx.write_word = ctx.sv_write_word = ctx.user_write_word = wr16;
    m68k_set_context(&ctx);
}

void cpu_reset(void) {
    for (int l = 1; l < 8; l++) m68k_lower_irq(l);
    cur_level = 0;
    want_level = want_vector = want_dirty = 0;
    m68k_reset();
    m68k_set_irq_type(NULL, 0);        /* levels stay up until lowered */
}


int32_t cpu_run(int32_t cycles) {
    unsigned before = m68kcontext.cycles_counter;
    run_start = before;
    running = 1;
    m68k_emulate(cycles);
    running = 0;
    if (want_dirty) apply_irq();
    return (int32_t)(m68kcontext.cycles_counter - before);
}

int32_t cpu_elapsed(void) {
    return running ? (int32_t)(m68k_get_cycles_counter() - run_start) : 0;
}

void cpu_end_slice(void) { if (running) m68k_stop_emulating(); }

/* FAME clears a level's pending bit when it takes the interrupt; the
   Neo Geo keeps the line up until the program acknowledges it, so the
   wanted level is (re)asserted whenever it changes. Changes requested
   from inside a memory handler are applied when m68k_emulate returns
   (FAME's IRQ calls are not made re-entrantly). */
static void apply_irq(void) {
    static int cur_vector;
    want_dirty = 0;
    if (want_level == cur_level && (want_level == 0 || want_vector == cur_vector)) return;
    for (int l = 1; l < 8; l++) m68k_lower_irq(l);
    if (want_level) m68k_raise_irq(want_level, want_vector);
    cur_level = want_level;
    cur_vector = want_vector;
}

void cpu_set_irq(int level, int vector) {
    want_level = level;
    want_vector = vector;
    want_dirty = 1;
    if (running) { m68k_stop_emulating(); return; }
    apply_irq();
}

uint32_t cpu_pc(void) { return m68k_get_pc(); }

#ifdef NGCD_DEBUG
#include <stdio.h>
void ngcd_debug_dump(void) {
    fprintf(stderr, "D:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", m68kcontext.dreg[i]);
    fprintf(stderr, "\nA:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", m68kcontext.areg[i]);
    fprintf(stderr, "\nSR %04X ASP %08X pending %X level %d\n", m68kcontext.sr, m68kcontext.asp, ng.pending_irq, cur_level);
    uint32_t sp = m68kcontext.areg[7];
    fprintf(stderr, "stack:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %04X", ngcd_read16(sp + 2 * i));
    fprintf(stderr, "\nvec 64..7C:"); for (uint32_t a = 0x64; a < 0x80; a += 4) fprintf(stderr, " %04X%04X", ngcd_read16(a), ngcd_read16(a + 2));
    fprintf(stderr, " vectors_rom %d\n", ng.vectors_rom);
}
#endif
#ifdef NGCD_DEBUG
int ngcd_fine;
void ngcd_debug_line(void) {
    static uint32_t last;
    uint32_t pc = m68k_get_pc();
    if (pc == last) return;
    last = pc;
    fprintf(stderr, "pc %06X sr %04X sp %08X d0 %08X a0 %08X irq %X\n", pc, m68kcontext.sr, m68kcontext.areg[7], m68kcontext.dreg[0], m68kcontext.areg[0], ng.pending_irq);
}
#endif

/* Host Z80 glue: superzazu/z80 (MIT) */
#include "../src/core/ngcd.h"
#include "z80.h"

uint8_t ngcd_z80_in(uint8_t port);
void    ngcd_z80_out(uint8_t port, uint8_t v);

static z80 cpu;
static int irq_line, running;
static unsigned long run_start;

static uint8_t rb(void *u, uint16_t a) { (void)u; return ng.z80ram[a]; }
static void wb(void *u, uint16_t a, uint8_t v) { (void)u; ng.z80ram[a] = v; }
static uint8_t pin(z80 *z, uint8_t p) { (void)z; return ngcd_z80_in(p); }
static void pout(z80 *z, uint8_t p, uint8_t v) { (void)z; ngcd_z80_out(p, v); }

void z80if_init(void) { }
void z80if_reset(void) {
    z80_init(&cpu);
    cpu.read_byte = rb; cpu.write_byte = wb; cpu.port_in = pin; cpu.port_out = pout;
    irq_line = 0;
}
int32_t z80if_run(int32_t cycles) {
    running = 1;
    run_start = cpu.cyc;
    while ((long)(cpu.cyc - run_start) < cycles) {
        if (irq_line && cpu.iff1 && !cpu.int_pending) z80_gen_int(&cpu, 0xFF);
        z80_step(&cpu);
    }
    running = 0;
    return (int32_t)(cpu.cyc - run_start);
}
int32_t z80if_elapsed(void) { return running ? (int32_t)(cpu.cyc - run_start) : 0; }
void z80if_nmi(void) { z80_gen_nmi(&cpu); }
void z80if_irq(int asserted) { irq_line = asserted; }

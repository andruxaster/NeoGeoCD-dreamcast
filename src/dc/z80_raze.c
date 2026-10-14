/* Dreamcast Z80 glue: RAZE-SH4. The sound CPU sees 64 KB of RAM and the
   YM2610 / 68000 latches on its I/O ports. */
#include "../core/ngcd.h"
#include "../cpu/raze/raze_api.h"

uint8_t ngcd_z80_in(uint8_t port);
void    ngcd_z80_out(uint8_t port, uint8_t v);

static int irq_line, running;

static uint8_t port_in(uint16_t p) { return ngcd_z80_in((uint8_t)p); }
static void port_out(uint16_t p, uint8_t v) { ngcd_z80_out((uint8_t)p, v); }

void z80if_init(void) { }

void z80if_reset(void) {
    z80_init_memmap();
    z80_map_fetch(0x0000, 0xFFFF, ng.z80ram);
    z80_map_read (0x0000, 0xFFFF, ng.z80ram);
    z80_map_write(0x0000, 0xFFFF, ng.z80ram);
    z80_set_in(port_in);
    z80_set_out(port_out);
    z80_end_memmap();
    z80_reset();
    irq_line = 0;
}

int32_t z80if_run(int32_t cycles) {
    if (irq_line) z80_raise_IRQ(0xFF);     /* level: re-assert (RAZE lowers on accept) */
    running = 1;
    int done = z80_emulate(cycles);
    running = 0;
    return done > 0 ? done : cycles;
}

int32_t z80if_elapsed(void) { return running ? z80_get_cycles_elapsed() : 0; }
void z80if_nmi(void) { z80_cause_NMI(); }
void z80if_irq(int asserted) {
    irq_line = asserted;
    if (asserted) z80_raise_IRQ(0xFF);
    else z80_lower_IRQ();
}

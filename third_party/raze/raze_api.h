/* RAZE-SH4 (Richard Mitton, SH4 rewrite by Oscar Orallo) C interface.
   raze.s is generated from MAME4ALL's raze.s.m4 with
   -DAUTODOWN_IRQS -DEMULATE_UNDOC_FLAGS -DNO_PC_LIMIT (no MAME hooks).
   Note: in this build the cycle counter lives in a register during
   emulation, so z80_skip_idle/z80_stop_emulating from a handler have no
   effect; idle skipping is done between z80_emulate calls instead.
   RAZE_LINUX_TEST maps the sh-elf underscored symbols for sh4-linux tests. */
#ifndef RAZE_API_H
#define RAZE_API_H
#include <stdint.h>

#ifdef RAZE_LINUX_TEST
#define RAZE_SYM(x) __asm__("_" #x)
#else
#define RAZE_SYM(x)
#endif

#define Z80_MAP_DIRECT  0
#define Z80_MAP_HANDLED 1

typedef enum {
    Z80_REG_AF = 0, Z80_REG_BC, Z80_REG_DE, Z80_REG_HL, Z80_REG_IX, Z80_REG_IY,
    Z80_REG_PC, Z80_REG_SP, Z80_REG_AF2, Z80_REG_BC2, Z80_REG_DE2, Z80_REG_HL2,
    Z80_REG_IFF1, Z80_REG_IFF2, Z80_REG_IR, Z80_REG_IM, Z80_REG_IRQVector,
    Z80_REG_IRQLine, Z80_REG_Halted
} z80_register;

void     z80_reset(void)                                           RAZE_SYM(z80_reset);
int      z80_emulate(int cycles)                                   RAZE_SYM(z80_emulate);
void     z80_raise_IRQ(uint8_t vector)                             RAZE_SYM(z80_raise_IRQ);
void     z80_lower_IRQ(void)                                       RAZE_SYM(z80_lower_IRQ);
void     z80_cause_NMI(void)                                       RAZE_SYM(z80_cause_NMI);
uint16_t z80_get_reg(z80_register reg)                             RAZE_SYM(z80_get_reg);
int      z80_get_cycles_elapsed(void)                              RAZE_SYM(z80_get_cycles_elapsed);
void     z80_skip_idle(void)                                       RAZE_SYM(z80_skip_idle);
void     z80_init_memmap(void)                                     RAZE_SYM(z80_init_memmap);
void     z80_map_fetch(uint16_t start, uint16_t end, uint8_t *mem) RAZE_SYM(z80_map_fetch);
void     z80_map_read(uint16_t start, uint16_t end, uint8_t *mem)  RAZE_SYM(z80_map_read);
void     z80_map_write(uint16_t start, uint16_t end, uint8_t *mem) RAZE_SYM(z80_map_write);
void     z80_add_read(uint16_t start, uint16_t end, int method, void *data)  RAZE_SYM(z80_add_read);
void     z80_add_write(uint16_t start, uint16_t end, int method, void *data) RAZE_SYM(z80_add_write);
void     z80_set_in(uint8_t (*handler)(uint16_t port))             RAZE_SYM(z80_set_in);
void     z80_set_out(void (*handler)(uint16_t port, uint8_t v))    RAZE_SYM(z80_set_out);
void     z80_end_memmap(void)                                      RAZE_SYM(z80_end_memmap);

#endif

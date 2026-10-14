/* Host 68000 glue: Musashi */
#include <stddef.h>
#include "../src/core/ngcd.h"
#include "musashi/m68k.h"

static int irq_vector;

int ngcd_int_ack(int level) { (void)level; return irq_vector; }

unsigned int m68k_read_memory_8(unsigned int a)  { return ngcd_read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return ngcd_read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return ((unsigned)ngcd_read16(a) << 16) | ngcd_read16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { ngcd_write8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { ngcd_write16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { ngcd_write16(a, (uint16_t)(v >> 16)); ngcd_write16(a + 2, (uint16_t)v); }
unsigned int m68k_read_disassembler_8(unsigned int a)  { return ngcd_read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return ngcd_read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

void cpu_init(void) { m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); }
void cpu_reset(void) { irq_vector = 0; m68k_set_irq(0); m68k_pulse_reset(); }
int32_t cpu_run(int32_t cycles) { return m68k_execute(cycles); }
int32_t cpu_elapsed(void) { return m68k_cycles_run(); }
void cpu_end_slice(void) { m68k_end_timeslice(); }
void cpu_set_irq(int level, int vector) { irq_vector = vector; m68k_set_irq(level); }
uint32_t cpu_pc(void) { return m68k_get_reg(NULL, M68K_REG_PC); }

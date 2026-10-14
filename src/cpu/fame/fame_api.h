/* Declarations for FAME SH4 2.1 (fame.s), taken from MAME4ALL's fame.h.
   FAME_LINUX_TEST: the assembler symbols carry the sh-elf leading
   underscore; under sh4-linux (qemu tests) C names have none, so map them. */
#ifndef FAME_API_H
#define FAME_API_H

#ifdef FAME_LINUX_TEST
#define FAME_SYM(x) __asm__("_" #x)
#else
#define FAME_SYM(x)
#endif

#define M68K_AUTOVECTORED_IRQ  -1
#define M68K_AUTO_LOWER_IRQ     1

struct M68K_PROGRAM {             /* host = offset + address */
    unsigned low_addr;
    unsigned high_addr;
    unsigned offset;
};

struct M68K_DATA {                /* data block (host = data + address) or handler */
    unsigned low_addr;
    unsigned high_addr;
    void    *mem_handler;
    void    *data;
};

struct M68K_CONTEXT {
    struct M68K_PROGRAM *fetch;
    struct M68K_DATA *read_byte;
    struct M68K_DATA *read_word;
    struct M68K_DATA *write_byte;
    struct M68K_DATA *write_word;
    struct M68K_PROGRAM *sv_fetch;
    struct M68K_DATA *sv_read_byte;
    struct M68K_DATA *sv_read_word;
    struct M68K_DATA *sv_write_byte;
    struct M68K_DATA *sv_write_word;
    struct M68K_PROGRAM *user_fetch;
    struct M68K_DATA *user_read_byte;
    struct M68K_DATA *user_read_word;
    struct M68K_DATA *user_write_byte;
    struct M68K_DATA *user_write_word;
    void           (*reset_handler)(void);
    void           (*iack_handler)(unsigned level);
    unsigned *     icust_handler;
    unsigned       dreg[8];
    unsigned       areg[8];
    unsigned       asp;
    unsigned       pc;
    unsigned       cycles_counter;
    unsigned char  interrupts[8];
    unsigned short sr;
    unsigned short execinfo;
};

extern struct M68K_CONTEXT m68kcontext FAME_SYM(m68kcontext);

void     m68k_init(void)                           FAME_SYM(m68k_init);
unsigned m68k_reset(void)                          FAME_SYM(m68k_reset);
unsigned m68k_emulate(int n)                       FAME_SYM(m68k_emulate);
unsigned m68k_get_pc(void)                         FAME_SYM(m68k_get_pc);
int      m68k_raise_irq(int level, int vector)     FAME_SYM(m68k_raise_irq);
int      m68k_lower_irq(int level)                 FAME_SYM(m68k_lower_irq);
void     m68k_set_irq_type(void *context, int type) FAME_SYM(m68k_set_irq_type);
void     m68k_set_context(void *context)           FAME_SYM(m68k_set_context);
void     m68k_get_context(void *context)           FAME_SYM(m68k_get_context);

#endif

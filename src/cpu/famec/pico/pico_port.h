/* the bits of PicoDrive's pico_port.h / pico_types.h FAME C uses */
#ifndef PICO_PORT_INCLUDED
#define PICO_PORT_INCLUDED
#include <stdlib.h>
#include <stdint.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32; typedef uintptr_t uptr;
#define INLINE static inline
/* little-endian host (x86, EE) */
#define MEM_BE2(a) ((a)^1)
#define MEM_BE4(a) ((a)^3)
#define MEM_LE2(a) (a)
#define MEM_LE4(a) (a)
#define NOINLINE __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect((x), 0)
#endif

/* NGA: CD audio tracks converted by tools/ngcdconv - IMA ADPCM, 44.1 kHz
   stereo, one 596-byte block per CD sector:
     [pred L s16][index L u8][0][pred R s16][index R u8][0] + 588 bytes,
     byte k = left code (low nibble) | right code (high nibble) of sample k.
   File: "NGA1", rate u32, channels u16, 0 u16, sectors u32, then blocks. */
#ifndef NGA_H
#define NGA_H
#include <stdint.h>

#define NGA_HEADER  16
#define NGA_BLOCK   596
#define NGA_SAMPLES 588

static const int16_t nga_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t nga_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int pred, idx; } nga_ch_t;

static inline int nga_decode(nga_ch_t *s, int code) {
    int step = nga_step[s->idx];
    int d = step >> 3;
    if (code & 4) d += step;
    if (code & 2) d += step >> 1;
    if (code & 1) d += step >> 2;
    int p = (code & 8) ? s->pred - d : s->pred + d;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    s->pred = p;
    int i = s->idx + nga_index[code];
    s->idx = i < 0 ? 0 : (i > 88 ? 88 : i);
    return p;
}

/* decode one block into 588 interleaved stereo samples */
static inline void nga_decode_block(const uint8_t *blk, int16_t *out) {
    nga_ch_t l = { (int16_t)(blk[0] | (blk[1] << 8)), blk[2] };
    nga_ch_t r = { (int16_t)(blk[4] | (blk[5] << 8)), blk[6] };
    const uint8_t *d = blk + 8;
    for (int k = 0; k < NGA_SAMPLES; k++) {
        out[2 * k]     = (int16_t)nga_decode(&l, d[k] & 15);
        out[2 * k + 1] = (int16_t)nga_decode(&r, d[k] >> 4);
    }
}
#endif

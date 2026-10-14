/* Neo Geo CD 68000 memory map, CD interface registers, DMA and the
   0xE00000 transfer window. After neocd_libretro memory*.cpp. */
#include <string.h>
#include "ngcd.h"

#define RD8(p, a)       ((p)[(a) ^ 1])
#define WR8(p, a, v)    ((p)[(a) ^ 1] = (uint8_t)(v))
#define RD16(p, a)      (*(uint16_t *)((p) + (a)))
#define WR16(p, a, v)   (*(uint16_t *)((p) + (a)) = (uint16_t)(v))

/* ------------------------------------------------------------ sprites */
static uint32_t spread[256];
static const uint8_t spr_shift[4] = { 1, 0, 3, 2 };

static void spr_tables(void) {
    for (int v = 0; v < 256; v++) {
        uint32_t s = 0;
        for (int i = 0; i < 8; i++) if (v & (1 << i)) s |= 1u << (4 * i);
        spread[v] = s;
    }
}

void ngcd_spr_write8(uint32_t off, uint8_t v) {
    ngcd_tile_gen[(off >> 7) & (NGCD_SPR_SIZE / 128 - 1)] = ngcd_vid_gen;
    uint32_t g = off >> 2, sh = spr_shift[off & 3];
    uint32_t m = 0x11111111u << sh;
    ng.spr[g] = (ng.spr[g] & ~m) | (spread[v] << sh);
}

/* one 16-bit write at an even offset: both bytes land in the same 32-bit
   word (shifts 1/0 or 3/2), pv = spread[hi] << 1 | spread[lo] */
static inline void spr_w16(uint32_t off, uint32_t pv) {
    ngcd_tile_gen[(off >> 7) & (NGCD_SPR_SIZE / 128 - 1)] = ngcd_vid_gen;
    uint32_t g = off >> 2, sh = off & 2;
    uint32_t m = 0x33333333u << sh;
    ng.spr[g] = (ng.spr[g] & ~m) | (pv << sh);
}
#define SPR_PV(v) ((spread[(v) >> 8] << 1) | spread[(v) & 0xFF])

uint8_t ngcd_spr_read8(uint32_t off) {
    uint32_t x = (ng.spr[off >> 2] >> spr_shift[off & 3]) & 0x11111111u;
    return (uint8_t)((x & 1) | ((x >> 3) & 2) | ((x >> 6) & 4) | ((x >> 9) & 8) |
                     ((x >> 12) & 16) | ((x >> 15) & 32) | ((x >> 18) & 64) | ((x >> 21) & 128));
}

/* ------------------------------------------------------------ vectors */
void ngcd_map_vectors(int rom) {
    if (rom == ng.vectors_rom) return;
    if (rom) {
        memcpy(ng.vec_save, ng.ram, 0x80);
        memcpy(ng.ram, ng.rom, 0x80);
    } else {
        memcpy(ng.ram, ng.vec_save, 0x80);
    }
    ng.vectors_rom = rom;
}

/* underlying RAM (DMA / loaders), regardless of the vector mapping */
static inline uint16_t ram_r16_under(uint32_t a) {
    if (a < 0x80 && ng.vectors_rom) return RD16(ng.vec_save, a);
    return RD16(ng.ram, a & 0x1FFFFF);
}
static inline void ram_w16_under(uint32_t a, uint16_t v) {
    if (a < 0x80 && ng.vectors_rom) { WR16(ng.vec_save, a, v); return; }
    WR16(ng.ram, a & 0x1FFFFF, v);
}

/* ------------------------------------------------------------ transfer window */
static int window_open(void) { return (ng.area_select & ng.bus_request) != 0; }

static uint8_t window_r8(uint32_t a) {
    if (!window_open()) return 0xFF;
    switch (ng.area_select) {
    case AREA_FIX: if (a & 1) return ng.fix[(a >> 1) & 0x1FFFF]; break;
    case AREA_SPR: return ngcd_spr_read8((a + (ng.spr_bank & 3) * 0x100000) & 0x3FFFFF);
    case AREA_Z80: if (a & 1) return ng.z80ram[(a >> 1) & 0xFFFF]; break;
    case AREA_PCM: if (a & 1) return ng.pcm[((a >> 1) + (ng.pcm_bank & 1) * 0x80000) & 0xFFFFF]; break;
    }
    return 0xFF;
}

static uint16_t window_r16(uint32_t a) {
    if (!window_open()) return 0xFFFF;
    switch (ng.area_select) {
    case AREA_FIX: return 0xFF00 | ng.fix[(a >> 1) & 0x1FFFF];
    case AREA_SPR: {
        uint32_t o = (a + (ng.spr_bank & 3) * 0x100000) & 0x3FFFFE;
        return (uint16_t)((ngcd_spr_read8(o) << 8) | ngcd_spr_read8(o + 1));
    }
    case AREA_Z80: return 0xFF00 | ng.z80ram[(a >> 1) & 0xFFFF];
    case AREA_PCM: return 0xFF00 | ng.pcm[((a >> 1) + (ng.pcm_bank & 1) * 0x80000) & 0xFFFFF];
    }
    return 0xFFFF;
}

static void window_w8(uint32_t a, uint8_t v) {
    if (!window_open()) return;
    switch (ng.area_select) {
    case AREA_FIX:
        if (a & 1) {
            uint32_t o = (a >> 1) & 0x1FFFF;
            ng.fix[o] = v;
            if (v) ng.fix_usage[o >> 5] = 1;
            ngcd_fix_gen = ngcd_fixdata_gen = ngcd_vid_gen;
        }
        break;
    case AREA_SPR: ngcd_spr_write8((a + (ng.spr_bank & 3) * 0x100000) & 0x3FFFFF, v); break;
    case AREA_Z80: if (a & 1) ng.z80ram[(a >> 1) & 0xFFFF] = v; break;
    case AREA_PCM: if (a & 1) ng.pcm[((a >> 1) + (ng.pcm_bank & 1) * 0x80000) & 0xFFFFF] = v; break;
    }
}

static void window_w16(uint32_t a, uint16_t v) {
    if (!window_open()) return;
    switch (ng.area_select) {
    case AREA_FIX: {
        uint32_t o = (a >> 1) & 0x1FFFF;
        ng.fix[o] = (uint8_t)v;
        if (v & 0xFF) ng.fix_usage[o >> 5] = 1;
        ngcd_fix_gen = ngcd_fixdata_gen = ngcd_vid_gen;
        break;
    }
    case AREA_SPR:
        spr_w16((a + (ng.spr_bank & 3) * 0x100000) & 0x3FFFFE, SPR_PV(v));
        break;
    case AREA_Z80: ng.z80ram[(a >> 1) & 0xFFFF] = (uint8_t)v; break;
    case AREA_PCM: ng.pcm[((a >> 1) + (ng.pcm_bank & 1) * 0x80000) & 0xFFFFF] = (uint8_t)v; break;
    }
}

/* ------------------------------------------------------------ CD interface */
static uint8_t cdif_r8(uint32_t a) {
    switch (a) {
    case 0x103: return lc_read_reg();
    case 0x161: return lc_read_response();
    }
    return 0x00;
}

static uint16_t cdif_r16(uint32_t a) {
    switch (a) {
    case 0x004: return (uint16_t)ng.irq_mask2;
    case 0x11C:
        if (ng.is_cdz) return (uint16_t)((~ng.nationality & 7) << 8);
        return (uint16_t)(((~ng.nationality & 7) << 8) | 0x1000);
    case 0x188: case 0x18A: return 0;      /* CD audio sample readback (CD player) */
    }
    return 0;
}

static void cdif_w8(uint32_t a, uint8_t v) {
    switch (a) {
    case 0x00F:
        if (v & 0x20) ng.pending_irq &= ~IRQ_CDDEC;
        if (v & 0x10) ng.pending_irq &= ~IRQ_CDCOMM;
        ngcd_update_irq();
        break;
    case 0x061:
        if (v == 0x40) ngcd_dma();
        else if (v == 0) ngcd_dma_reset();
        break;
    case 0x101: ng.lc_regptr = v; break;
    case 0x103: lc_write_reg(v); break;
    case 0x105:
        switch (v) {
        case 0: ng.area_select = AREA_SPR; break;
        case 1: ng.area_select = AREA_PCM; break;
        case 4: ng.area_select = AREA_Z80; break;
        case 5: ng.area_select = AREA_FIX; break;
        default: ng.area_select = 0; break;
        }
        break;
    case 0x111: ng.spr_disable = (v != 0); break;
    case 0x115: ng.fix_disable = (v != 0); break;
    case 0x119: ng.video_enable = (v != 0); break;
    case 0x121: ng.bus_request |= AREA_SPR; break;
    case 0x123: ng.bus_request |= AREA_PCM; break;
    case 0x127: ng.bus_request |= AREA_Z80; break;
    case 0x129: ng.bus_request |= AREA_FIX; break;
    case 0x141: ng.bus_request &= ~AREA_SPR; break;
    case 0x143: ng.bus_request &= ~AREA_PCM; break;
    case 0x147: ng.bus_request &= ~AREA_Z80; break;
    case 0x149: ng.bus_request &= ~AREA_FIX; ngcd_fix_usage_update(); break;
    case 0x163: lc_write_command(v); break;
    case 0x165: lc_packet_pointer(v); break;
    case 0x16F: break;                       /* watchdog enable: not emulated */
    case 0x181:
        ng.cd_comm_nreset = (v != 0);
        lc_reset_packet_pointers();
        break;
    case 0x183: {
        extern void ngcd_z80_enable(int on);
        ngcd_z80_enable(v != 0);
        break;
    }
    case 0x1A1: ng.spr_bank = v; break;
    case 0x1A3: ng.pcm_bank = v; break;
    }
}

static void cdif_w16(uint32_t a, uint16_t v) {
    switch (a) {
    case 0x000: ngcd_cd_stop(); ng.cd_status = 0x00; break;
    case 0x002: ng.irq_mask1 = v; ngcd_update_irq(); break;
    case 0x004: ng.irq_mask2 = v; ngcd_update_irq(); break;
    case 0x064: ng.dma_dst = (ng.dma_dst & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x066: ng.dma_dst = (ng.dma_dst & 0xFFFF0000u) | v; break;
    case 0x068: ng.dma_src = (ng.dma_src & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x06A: ng.dma_src = (ng.dma_src & 0xFFFF0000u) | v; break;
    case 0x06C: ng.dma_pattern = v; break;
    case 0x070: ng.dma_len = (ng.dma_len & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x072: ng.dma_len = (ng.dma_len & 0xFFFF0000u) | v; break;
    default:
        if (a >= 0x07E && a <= 0x08E) ng.dma_config[(a - 0x07E) >> 1] = v;
        break;
    }
}

/* ------------------------------------------------------------ misc ports */
static void switch_w(uint32_t a) {
    switch (a & 0x1E) {
    case 0x00: if (ng.shadow) { ng.shadow = 0; ngcd_pal_convert_all(); } break;
    case 0x10: if (!ng.shadow) { ng.shadow = 1; ngcd_pal_convert_all(); } break;
    case 0x02: ngcd_map_vectors(1); break;
    case 0x0E: ng.pal_bank = 0; ngcd_fix_gen = ngcd_vid_gen; break;
    case 0x12: ngcd_map_vectors(0); break;
    case 0x1E: ng.pal_bank = 1; ngcd_fix_gen = ngcd_vid_gen; break;
    }
}

static void audio_command_w(uint8_t v) {
    ng.t_user[T_AUDIOCMD] = v;
    ngcd_timer_arm(T_AUDIOCMD, 1);
    cpu_end_slice();
}

static int input_selected(void) {
    return ng.input_sel == 0x00 || ng.input_sel == 0x12 || ng.input_sel == 0x1B;
}

/* ------------------------------------------------------------ 68000 accessors */
uint8_t ngcd_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x200000) return RD8(ng.ram, a);
    switch (a >> 16) {
    case 0x30: case 0x31: return (!(a & 1) && input_selected()) ? ng.input1 : 0xFF;
    case 0x32: case 0x33: return (!(a & 1)) ? (uint8_t)ng.audio_result : 0xFF;
    case 0x34: case 0x35: return (!(a & 1) && input_selected()) ? ng.input2 : 0xFF;
    case 0x38: case 0x39: if (a & 1) return 0xFF; return input_selected() ? ng.input3 : 0x0F;
    case 0x3C: case 0x3D: return (a & 1) ? 0xFF : (uint8_t)(ngcd_lspc_read(a & 0x6) >> 8);
    }
    if (a >= 0x400000 && a < 0x500000) {
        uint16_t w = ng.palram[ng.pal_bank * 4096 + ((a & 0x1FFF) >> 1)];
        return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
    }
    if (a >= 0x800000 && a < 0x900000) return (a & 1) ? ng.backup[(a & 0x3FFF) >> 1] : 0xFF;
    if (a >= 0xC00000 && a < 0xD00000) return RD8(ng.rom, a & 0x7FFFF);
    if (a >= 0xE00000 && a < 0xF00000) return window_r8(a & 0xFFFFF);
    if (a >= 0xFF0000 && a < 0xFF0200) return cdif_r8(a & 0x1FF);
    return 0xFF;
}

uint16_t ngcd_read16(uint32_t a) {
    a &= 0xFFFFFE;
    if (a < 0x200000) return RD16(ng.ram, a);
    switch (a >> 16) {
    case 0x30: case 0x31: return input_selected() ? (uint16_t)((ng.input1 << 8) | 0xFF) : 0xFFFF;
    case 0x32: case 0x33: return (uint16_t)((ng.audio_result << 8) | 0xFF);
    case 0x34: case 0x35: return input_selected() ? (uint16_t)((ng.input2 << 8) | 0xFF) : 0xFFFF;
    case 0x38: case 0x39: return input_selected() ? (uint16_t)((ng.input3 << 8) | 0xFF) : 0x0FFF;
    case 0x3C: case 0x3D: return ngcd_lspc_read(a & 0xE);
    }
    if (a >= 0x400000 && a < 0x500000) return ng.palram[ng.pal_bank * 4096 + ((a & 0x1FFF) >> 1)];
    if (a >= 0x800000 && a < 0x900000) return 0xFF00 | ng.backup[(a & 0x3FFF) >> 1];
    if (a >= 0xC00000 && a < 0xD00000) return RD16(ng.rom, a & 0x7FFFF);
    if (a >= 0xE00000 && a < 0xF00000) return window_r16(a & 0xFFFFF);
    if (a >= 0xFF0000 && a < 0xFF0200) return cdif_r16(a & 0x1FF);
    return 0xFFFF;
}

void ngcd_write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a < 0x200000) {
        if (a < 0x80 && ng.vectors_rom) return;
        WR8(ng.ram, a, v);
        return;
    }
    switch (a >> 16) {
    case 0x30: case 0x31: return;                         /* watchdog kick */
    case 0x32: case 0x33: if (!(a & 1)) audio_command_w(v); return;
    case 0x38: case 0x39: if (a & 1) ng.input_sel = v; return;
    case 0x3A: case 0x3B: if (a & 1) switch_w(a & 0x1E); return;
    case 0x3C: case 0x3D: if (!(a & 1)) ngcd_lspc_write(a & 0xE, (uint16_t)((v << 8) | v)); return;
    }
    if (a >= 0x400000 && a < 0x500000) {
        uint32_t idx = ng.pal_bank * 4096 + ((a & 0x1FFF) >> 1);
        uint16_t w = ng.palram[idx];
        w = (a & 1) ? (uint16_t)((w & 0xFF00) | v) : (uint16_t)((w & 0x00FF) | (v << 8));
        ngcd_pal_write(idx, w);
        return;
    }
    if (a >= 0x800000 && a < 0x900000) { if (a & 1) ng.backup[(a & 0x3FFF) >> 1] = v; return; }
    if (a >= 0xE00000 && a < 0xF00000) { window_w8(a & 0xFFFFF, v); return; }
    if (a >= 0xFF0000 && a < 0xFF0200) { cdif_w8(a & 0x1FF, v); return; }
}

void ngcd_write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    if (a < 0x200000) {
        if (a < 0x80 && ng.vectors_rom) return;
        WR16(ng.ram, a, v);
        return;
    }
    switch (a >> 16) {
    case 0x30: case 0x31: return;
    case 0x32: case 0x33: audio_command_w((uint8_t)(v >> 8)); return;
    case 0x38: case 0x39: ng.input_sel = (uint8_t)v; return;
    case 0x3A: case 0x3B: switch_w(a & 0x1E); return;
    case 0x3C: case 0x3D: ngcd_lspc_write(a & 0xE, v); return;
    }
    if (a >= 0x400000 && a < 0x500000) { ngcd_pal_write(ng.pal_bank * 4096 + ((a & 0x1FFF) >> 1), v); return; }
    if (a >= 0x800000 && a < 0x900000) { ng.backup[(a & 0x3FFF) >> 1] = (uint8_t)v; return; }
    if (a >= 0xE00000 && a < 0xF00000) { window_w16(a & 0xFFFFF, v); return; }
    if (a >= 0xFF0000 && a < 0xFF0200) { cdif_w16(a & 0x1FF, v); return; }
}

/* ------------------------------------------------------------ DMA */
static uint16_t dma_r16(uint32_t a) {
    a &= 0xFFFFFE;
    if (a < 0x200000) return ram_r16_under(a);
    return ngcd_read16(a);
}

static void dma_w16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    if (a < 0x200000) { ram_w16_under(a, v); return; }
    if (a >= 0xC00000 && a < 0xD00000) return;
    if (a >= 0xE00000 && a < 0xF00000) { window_w16(a & 0xFFFFF, v); return; }
    ngcd_write16(a, v);
}

/* pattern fill through the 0xE00000 window (games clear PCM / sprite RAM
   this way, up to 1 MB at once): the same writes as the generic dma_w16
   loop, with the area switch taken once. Advances *d / *len up to the end
   of the window; the caller finishes the rest. */
static void window_fill(uint32_t *pd, uint32_t *plen) {
    uint32_t d = *pd, len = *plen, v = ng.dma_pattern;
    uint32_t n = (0xF00000 - d) >> 1;
    if (n > len) n = len;
    *pd = d + 2 * n; *plen = len - n;
    if (!window_open() || !n) return;
    uint32_t a = d & 0xFFFFF;
    switch (ng.area_select) {
    case AREA_FIX:
        for (; n; n--, a += 2) {
            uint32_t o = (a >> 1) & 0x1FFFF;
            ng.fix[o] = (uint8_t)v;
            if (v & 0xFF) ng.fix_usage[o >> 5] = 1;
        }
        ngcd_fix_gen = ngcd_fixdata_gen = ngcd_vid_gen;
        break;
    case AREA_SPR: {
        uint32_t o = a + (ng.spr_bank & 3) * 0x100000, pv = SPR_PV(v), gen = ngcd_vid_gen;
        uint32_t *spr = ng.spr, *tg = ngcd_tile_gen;
        if (o & 2) { spr_w16(o, pv); o += 2; n--; }
        for (; n >= 2; n -= 2, o += 4) { tg[o >> 7] = gen; spr[o >> 2] = pv | (pv << 2); }
        if (n) spr_w16(o, pv);
        break;
    }
    case AREA_Z80:
        for (; n; n--, a += 2) ng.z80ram[(a >> 1) & 0xFFFF] = (uint8_t)v;
        break;
    case AREA_PCM: {
        uint32_t bank = (ng.pcm_bank & 1) * 0x80000;
        for (; n; n--, a += 2) ng.pcm[((a >> 1) + bank) & 0xFFFFF] = (uint8_t)v;
        break;
    }
    }
}

/* word copy from 68000 RAM into the window (program / sample loads): the
   same writes as the generic loop, area switch taken once. Handles the run
   where the source stays in RAM above the vectors and the destination stays
   inside the window; the caller finishes the rest. */
static void window_copy(uint32_t *ps, uint32_t *pd, uint32_t *plen) {
    uint32_t s = *ps & 0xFFFFFE, d = *pd & 0xFFFFFE, len = *plen;
    if (s < 0x80 || s >= 0x200000) return;
    uint32_t n = (0xF00000 - d) >> 1, m = (0x200000 - s) >> 1;
    if (n > m) n = m;
    if (n > len) n = len;
    *ps = s + 2 * n; *pd = d + 2 * n; *plen = len - n;
    if (!window_open() || !n) return;
    const uint8_t *src = ng.ram + s;
    uint32_t a = d & 0xFFFFF;
    switch (ng.area_select) {
    case AREA_FIX:
        for (; n; n--, a += 2, src += 2) {
            uint32_t o = (a >> 1) & 0x1FFFF, v = *(const uint16_t *)src;
            ng.fix[o] = (uint8_t)v;
            if (v & 0xFF) ng.fix_usage[o >> 5] = 1;
        }
        ngcd_fix_gen = ngcd_fixdata_gen = ngcd_vid_gen;
        break;
    case AREA_SPR: {
        /* the window (1 MB) plus the bank offset stays below 4 MB, so the
           sprite offset is linear; a 4-byte aligned pair of words fills one
           whole decoded word */
        uint32_t o = a + (ng.spr_bank & 3) * 0x100000, gen = ngcd_vid_gen;
        uint32_t *spr = ng.spr, *tg = ngcd_tile_gen;
        const uint16_t *w = (const uint16_t *)src;
        if (o & 2) { spr_w16(o, SPR_PV(w[0])); o += 2; w++; n--; }
        for (; n >= 2; n -= 2, o += 4, w += 2) {
            tg[o >> 7] = gen;
            spr[o >> 2] = SPR_PV(w[0]) | (SPR_PV(w[1]) << 2);
        }
        if (n) spr_w16(o, SPR_PV(w[0]));
        break;
    }
    case AREA_Z80:
        for (; n; n--, a += 2, src += 2) ng.z80ram[(a >> 1) & 0xFFFF] = (uint8_t)*(const uint16_t *)src;
        break;
    case AREA_PCM: {
        uint32_t bank = (ng.pcm_bank & 1) * 0x80000;
        for (; n; n--, a += 2, src += 2) ng.pcm[((a >> 1) + bank) & 0xFFFFF] = (uint8_t)*(const uint16_t *)src;
        break;
    }
    }
}

static void dma_copy_cdrom(int odd) {
    uint32_t len = ng.dma_len, d = ng.dma_dst;
    if (len > 0x400) {
        /* Art of Fighting / CDZ quirk, as neocd_libretro */
        dma_w16(0x10FEFC, 0x0000);
        dma_w16(0x10FEFE, 0x0800);
        len = 0x400;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint16_t w = (uint16_t)((ng.lcbuf[2 * i] << 8) | ng.lcbuf[2 * i + 1]);
        if (odd) { dma_w16(d, w >> 8); d += 2; dma_w16(d, w); d += 2; }
        else     { dma_w16(d, w); d += 2; }
    }
    lc_end_transfer();
}

void ngcd_dma(void) {
    uint32_t len = ng.dma_len;
    switch (ng.dma_config[0]) {
    case 0xFE3D: case 0xFE6D: {                 /* copy: source in dst register */
        uint32_t s = ng.dma_dst, d = ng.dma_src;
        if ((d & 0xFFFFFE) >= 0xE00000 && (d & 0xFFFFFE) < 0xF00000) window_copy(&s, &d, &len);
        while (len--) { dma_w16(d, dma_r16(s)); s += 2; d += 2; }
        break;
    }
    case 0xFFC5: case 0xFF89: dma_copy_cdrom(0); break;
    case 0xFC2D:              dma_copy_cdrom(1); break;
    case 0xFEF5: {                              /* fill with addresses */
        uint32_t a = ng.dma_dst, d = ng.dma_dst;
        while (len--) { dma_w16(d, (uint16_t)(a >> 16)); d += 2; dma_w16(d, (uint16_t)a); d += 2; a += 4; }
        break;
    }
    case 0xFFCD: case 0xFFDD: {                 /* pattern */
        uint32_t d = ng.dma_dst & 0xFFFFFE;
        if (d >= 0xE00000 && d < 0xF00000) window_fill(&d, &len);
        while (len--) { dma_w16(d, ng.dma_pattern); d += 2; }
        break;
    }
    case 0xE2DD: case 0xF2DD: {                 /* copy to odd bytes */
        uint32_t s = ng.dma_dst, d = ng.dma_src;
        while (len--) {
            uint16_t w = dma_r16(s); s += 2;
            dma_w16(d, (uint16_t)((w >> 8) | (w << 8))); d += 2;
            dma_w16(d, w); d += 2;
        }
        break;
    }
    case 0xCFFD: {                              /* fill odd bytes with addresses */
        uint32_t a = ng.dma_dst, d = ng.dma_dst;
        while (len--) {
            dma_w16(d, (uint16_t)(a >> 24)); d += 2;
            dma_w16(d, (uint16_t)(a >> 16)); d += 2;
            dma_w16(d, (uint16_t)(a >> 8)); d += 2;
            dma_w16(d, (uint16_t)a); d += 2;
            a += 8;
        }
        break;
    }
    default:
        break;
    }
}

void ngcd_dma_reset(void) {
    memset(ng.dma_config, 0, sizeof(ng.dma_config));
    ng.dma_src = ng.dma_dst = ng.dma_len = 0;
    ng.dma_pattern = 0;
}

void ngcd_mem_reset(void) {
    static int tables;
    if (!tables) { spr_tables(); tables = 1; }
    memset(ng.ram, 0, NGCD_RAM_SIZE);
    memset(ng.spr, 0, NGCD_SPR_SIZE);
    memset(ng.fix, 0, NGCD_FIX_SIZE);
    ngcd_fixdata_gen = ngcd_vid_gen;
    memset(ng.pcm, 0, NGCD_PCM_SIZE);
    memset(ng.vram, 0, NGCD_VRAM_WORDS * 2);
    memset(ng.palram, 0, sizeof(ng.palram));
    memset(ng.z80ram, 0, NGCD_Z80_SIZE);
    ng.vectors_rom = 0;
    ngcd_map_vectors(1);
    ngcd_dma_reset();
    ng.bus_request = ng.area_select = ng.spr_bank = ng.pcm_bank = 0;
}

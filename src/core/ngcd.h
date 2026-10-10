/* Neo Geo CD (CDZ) emulation core for NGCD-DC (from dc-poker r13).
   Machine behaviour (memory map, CD interface, LC8951, CD drive protocol,
   timers, LSPC) follows NeoCD / neocd_libretro by Fabrice Martinez
   (LGPL-3.0), reimplemented in C; see THIRD_PARTY.md for inherited notices. */
#ifndef NGCD_H
#define NGCD_H
#include <stdint.h>

/* clocks (neocd_libretro timer.h) */
#define NGCD_MASTER_CLOCK     24168000
#define NGCD_SCREEN_W_PX      384
#define NGCD_SCREEN_H_PX      264
#define NGCD_CYCLES_PER_FRAME (4 * NGCD_SCREEN_W_PX * NGCD_SCREEN_H_PX)  /* master */
#define NGCD_ACTIVE_TOP       16
#define NGCD_ACTIVE_BOTTOM    240
#define NGCD_ACTIVE_LEFT      28
#define NGCD_FB_W             320
#define NGCD_FB_H             224

#define M2PIX(c)   ((c) / 4)            /* master -> pixel clock */
#define PIX2M(p)   ((p) * 4)
#define M2CPU(c)   ((c) / 2)            /* master -> 68000 */
#define CPU2M(c)   ((c) * 2)
#define M2Z80(c)   ((c) / 6)            /* master -> Z80 */
#define Z802M(c)   ((c) * 6)

/* memory sizes */
#define NGCD_RAM_SIZE     0x200000
#define NGCD_ROM_SIZE     0x80000
#define NGCD_SPR_SIZE     0x400000
#define NGCD_FIX_SIZE     0x20000
#define NGCD_PCM_SIZE     0x100000
#define NGCD_Z80_SIZE     0x10000
#define NGCD_VRAM_WORDS   0x10000
#define NGCD_BACKUP_SIZE  0x2000

/* interrupts */
enum { IRQ_VBL = 1, IRQ_CDDEC = 2, IRQ_CDCOMM = 4, IRQ_RASTER = 8 };

/* areas for the 0xE00000 window */
enum { AREA_SPR = 1, AREA_PCM = 2, AREA_Z80 = 4, AREA_FIX = 8 };

/* timers */
enum { T_VBL, T_HBL, T_VBLRELOAD, T_CDROM, T_YMA, T_YMB, T_AUDIOCMD, T_COUNT };

typedef struct { int active; int32_t delay; } ngcd_timer_t;

/* ------------------------------------------------------------ disc */
typedef struct {
    uint8_t  track, index, is_data;
    uint32_t start;      /* sector, 0 = start of track 1 */
    uint32_t length;
} ngcd_toc_entry_t;

typedef struct {
    int      n;
    ngcd_toc_entry_t e[200];
    uint32_t total;      /* leadout sector */
    uint8_t  first, last;
} ngcd_toc_t;

/* host disc backend */
typedef struct {
    /* data track 1, 2048-byte sector at (sector - start of track 1) */
    int  (*read_sector)(uint32_t sector, uint8_t *buf);
    /* CD-DA: start playing 'track' from 'offset' sectors after its index 1
       (negative offset: inside the pregap, start after -offset sectors of silence) */
    void (*audio_play)(int track, int32_t offset);
    void (*audio_stop)(void);
} ngcd_disc_io_t;

/* ------------------------------------------------------------ state */
typedef struct {
    /* memory: 68000 RAM and BIOS are stored as little-endian 16-bit words
       (byte at 68k address A lives at [A ^ 1]) - FAME's native layout */
    uint8_t  *ram, *rom;
    uint32_t *spr;              /* decoded sprites: 8 pixels (nibbles) per word */
    uint8_t  *fix, *pcm, *z80ram, *backup;
    uint16_t *vram;
    uint16_t  palram[2 * 4096];
    uint16_t  pal565[2 * 4096];
    uint8_t   yzoom[0x10000];
    uint8_t   vec_save[0x80];    /* RAM under the vectors while ROM is mapped */
    int       vectors_rom;

    /* DMA */
    uint16_t  dma_config[9];
    uint32_t  dma_src, dma_dst, dma_len;
    uint16_t  dma_pattern;
    uint32_t  spr_bank, pcm_bank, bus_request, area_select;

    /* machine */
    uint32_t  irq_mask1, irq_mask2, pending_irq, cd_vector;
    int       cd_comm_nreset;
    int       z80_disable, z80_nmi_disable;
    uint32_t  audio_command, audio_result;
    int       is_cdz;
    uint32_t  nationality;
    uint8_t   input1, input2, input3, input_sel;

    /* timing */
    ngcd_timer_t t[T_COUNT];
    uint32_t  t_user[T_COUNT];
    int32_t   remaining;        /* master cycles left in this frame */
    int32_t   slice_len;        /* master cycles of the current 68k slice */
    int32_t   z80_slice;        /* master cycles owed to the Z80 */
    uint64_t  time;             /* master cycles since reset */
    int       in_slice;

    /* video (LSPC) */
    uint32_t  vram_offset, vram_modulo, vram_data;
    uint32_t  aa_counter, aa_speed, aa_frame_counter;
    int       aa_disabled;
    int       spr_disable, fix_disable, video_enable, shadow;
    uint32_t  pal_bank;
    uint32_t  hirq_control, hirq_register;
    uint8_t   fix_usage[NGCD_FIX_SIZE / 32];
    int       frame_ready;

    /* LC8951 */
    uint32_t  lc_regptr;
    uint8_t   cmd[5], resp[5];
    uint32_t  cmd_ptr, resp_ptr, strobe;
    uint8_t   SBOUT, IFCTRL, DBCL, DBCH, DACL, DACH, DTRG, DTACK, WAL, WAH,
              CTRL0, CTRL1, PTL, PTH, COMIN, IFSTAT, HEAD0, HEAD1, HEAD2, HEAD3,
              STAT0, STAT1, STAT2, STAT3;
    uint8_t   lcbuf[2048];
    uint32_t  cd_status;        /* controller status */

    /* drive */
    ngcd_toc_t toc;
    uint32_t  cd_pos;
    int       cd_playing;
    int       cd_track_idx;     /* index into toc.e, -1 none */
    int       audio_track_playing;   /* track the backend plays, 0 none */
    int       cd_speed_mul;     /* data read speed multiplier (1 = real CDZ) */

    const ngcd_disc_io_t *io;

    /* stats */
    uint32_t  sectors_read;
} ngcd_t;

extern ngcd_t ng;

/* change tracking for renderers that cache converted graphics (PVR path):
   writes stamp the current generation, the renderer bumps it per frame */
extern uint32_t ngcd_vid_gen;
extern uint32_t ngcd_tile_gen[NGCD_SPR_SIZE / 128];   /* per 16x16 sprite tile */
extern uint32_t ngcd_pal_gen[512];                   /* per palette, both banks */
extern uint32_t ngcd_fix_gen;                        /* fix layer (map, tiles, palettes) */

/* ------------------------------------------------------------ API */
/* bios: 512 KB neocd.bin (either byte order), lo: 64 KB 000-lo.lo or NULL */
int  ngcd_init(const uint8_t *bios, const uint8_t *lo, const ngcd_toc_t *toc,
               const ngcd_disc_io_t *io);
void ngcd_reset(void);
void ngcd_run_frame(void);                  /* one 59.6 Hz frame */
void ngcd_set_input(uint8_t p1, uint8_t p2, uint8_t sys);  /* active low */
/* render the current screen into a 320x224 RGB565 buffer */
void ngcd_render(uint16_t *dst, int pitch);
/* audio: YM2610 stereo samples generated during the last frame */
int  ngcd_audio_take(int16_t *dst, int max_frames);
int  ngcd_audio_avail(void);                 /* stereo frames waiting */

/* hardware rendering support (see video.c): screen rectangles of sprite
   tile rows, coordinates in the 320x224 picture, v in tile rows 0..16 */
typedef struct {
    int16_t  x0, y0, x1, y1;
    uint16_t tile, pal;          /* sprite tile number, palette 0..511 (bank*256) */
    uint8_t  v0, v1, hflip, pad;
} ngcd_quad_t;
int      ngcd_build_quads(ngcd_quad_t *out, int max);
void     ngcd_fix_row_raw(uint16_t *out, int row, const uint16_t *pal1555);
uint16_t ngcd_color_1555(uint16_t neo_color);

/* ------------------------------------------------------------ internals */
void ngcd_update_irq(void);
void ngcd_timer_arm(int t, int32_t delay);
void ngcd_timer_arm_rel(int t, int32_t delay);
int  ngcd_screen_y(void);
int32_t ngcd_slice_elapsed(void);           /* master cycles into the slice */

/* memory (mem.c) */
uint8_t  ngcd_read8(uint32_t a);
uint16_t ngcd_read16(uint32_t a);
void     ngcd_write8(uint32_t a, uint8_t v);
void     ngcd_write16(uint32_t a, uint16_t v);
void     ngcd_map_vectors(int rom);
void     ngcd_dma(void);
void     ngcd_dma_reset(void);
uint8_t  ngcd_spr_read8(uint32_t off);
void     ngcd_spr_write8(uint32_t off, uint8_t v);
void     ngcd_mem_reset(void);

/* CD (cd.c) */
void    ngcd_cd_reset(void);
uint8_t lc_read_reg(void);
void    lc_write_reg(uint8_t v);
uint8_t lc_read_response(void);
void    lc_write_command(uint8_t v);
void    lc_packet_pointer(uint8_t v);
void    lc_reset_packet_pointers(void);
void    lc_end_transfer(void);
void    ngcd_cd_tick(void);                 /* CD timer callback */
void    ngcd_cd_stop(void);
int     ngcd_cd_is_playing(void);
int     ngcd_cd_is_data(void);

/* video (video.c) */
void     ngcd_video_reset(void);
uint16_t ngcd_lspc_read(uint32_t a);
void     ngcd_lspc_write(uint32_t a, uint16_t v);
void     ngcd_pal_write(uint32_t idx, uint16_t v);
void     ngcd_pal_convert_all(void);
void     ngcd_fix_usage_update(void);
void     ngcd_vbl_video(void);              /* auto animation at VBL */

/* CPU abstraction (cpu_*.c) */
void     cpu_init(void);
void     cpu_reset(void);
int32_t  cpu_run(int32_t cycles);           /* returns 68k cycles executed */
int32_t  cpu_elapsed(void);                 /* 68k cycles into the current run */
void     cpu_end_slice(void);
void     cpu_set_irq(int level, int vector); /* level 0 = none */
uint32_t cpu_pc(void);

/* Z80 abstraction (z80_*.c) */
void     z80if_init(void);
void     z80if_reset(void);
int32_t  z80if_run(int32_t cycles);         /* returns cycles executed */
void     z80if_nmi(void);
void     z80if_irq(int asserted);
int32_t  z80if_elapsed(void);

#endif

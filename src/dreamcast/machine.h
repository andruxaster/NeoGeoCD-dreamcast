/* Interface between the NGCD-DC frontend and its Neo Geo CD adapter. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>

typedef struct {
    const char *name;       /* file name in DATA_DIR (upper case, 8.3) */
    uint32_t    size;       /* expected size, 0 = any */
    uint32_t    alt_size;   /* also accepted (0 = none) */
    int         required;
} mach_file_t;

typedef struct {
    const char *title;
    int width, height;          /* visible picture */
    int tex_w, tex_h;           /* power-of-two texture holding the picture */
    int fill_screen;            /* 0: integer 2x centred, 1: stretch to 640x480 */
    int bilinear;               /* texture filtering */
    int audio_rate;             /* output sample rate, 0 = 44100 */
    int audio_stereo;           /* 1: audio() fills interleaved L/R pairs */
    int interframe_blend;       /* 1: blend each frame with the previous (R toggles) */
    int video_240p;             /* 1: 320x240 progressive video, picture shown 1:1
                                   (every frame whole on every field: 60 Hz flicker
                                   effects look as on the original hardware) */
    const mach_file_t *files;
    int nfiles;

    /* data[i] is NULL for a missing optional file; sizes[i] = loaded size */
    void     (*init)(uint8_t *const *data, const uint32_t *sizes);
    /* raw Dreamcast pad: buttons (CONT_*), analog triggers 0..255 */
    void     (*set_pad)(uint32_t buttons, int ltrig, int rtrig);
    /* optional: multi-player machines get every controller (port 0..3);
       a missing controller is reported with buttons = 0 */
    void     (*set_pad_n)(int port, uint32_t buttons, int ltrig, int rtrig);
    void     (*run_frame)(void);                        /* 1/60 s of machine time */
    uint64_t (*render_dirty)(uint16_t *dst, int pitch); /* bit n = 8-line row n redrawn */
    /* optional hardware rendering: pvr_init() after the PVR is set up
       (0 = ok), pvr_draw() submits the lists of one frame between
       pvr_scene_begin and pvr_scene_finish. R toggles it with render_dirty. */
    int      (*pvr_init)(void);
    void     (*pvr_draw)(void);
    /* n = number of int16 values (stereo: 2 per sample frame) */
    void     (*audio)(int16_t *out, int n, uint32_t rate);
} machine_t;

extern const machine_t machine;

#endif

/* Interface between the Dreamcast frontend and a single-board machine core. */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>

typedef struct {
    const char *name;       /* file name in the game folder (upper case, 8.3) */
    uint32_t    size;       /* expected size, 0 = any */
    uint32_t    alt_size;   /* also accepted (0 = none) */
    int         required;
} mach_file_t;

typedef struct {
    const char *title;
    int audio_rate;             /* output sample rate, 0 = 44100 */
    int audio_stereo;           /* 1: audio() fills interleaved L/R pairs */
    const mach_file_t *files;
    int nfiles;

    /* data[i] is NULL for a missing optional file; sizes[i] = loaded size */
    void     (*init)(uint8_t *const *data, const uint32_t *sizes);
    /* every controller (port 0..3): buttons (CONT_*), analog triggers 0..255;
       a missing controller is reported with buttons = 0 */
    void     (*set_pad_n)(int port, uint32_t buttons, int ltrig, int rtrig);
    void     (*run_frame)(void);                        /* 1/60 s of machine time */
    /* the picture is drawn by the PVR: pvr_init() after the PVR is set up
       (0 = ok), pvr_draw() submits the lists of one frame between
       pvr_scene_begin and pvr_scene_finish (320x240 screen) */
    int      (*pvr_init)(void);
    void     (*pvr_draw)(void);
    /* n = number of int16 values (stereo: 2 per sample frame) */
    void     (*audio)(int16_t *out, int n, uint32_t rate);
} machine_t;

extern const machine_t machine;

/* folder of the game on the disc, "/cd/" or e.g. "/cd/SSII/" (with the
   trailing slash): the anthology menu writes it into the loaded image
   (see main.c) */
extern const char *disc_dir;

#endif

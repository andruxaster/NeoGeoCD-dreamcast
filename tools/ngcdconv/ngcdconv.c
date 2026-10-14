/* ngcdconv - convert a Neo Geo CD image (CUE + BIN, one file per track or a
   single file) into the ngcd-dc disc layout:

     TOC.BIN       track/index table (same layout as the emulator expects)
     DATA.ISO      data track 1 as 2048-byte sectors
     TRKnn.NGA     each audio track (from INDEX 01), IMA ADPCM 44.1 kHz stereo,
                   one block per CD sector (588 samples) with decoder state,
                   so playback can start at any sector

   Build: cc -O2 -o ngcdconv ngcdconv.c
   Usage: ngcdconv game.cue outdir                                     */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define fseek64 fseeko
#define ftell64 ftello
#define make_dir(p) mkdir(p, 0755)
#endif

#define MAXF 128
#define MAXE 256

typedef struct { char path[1024]; uint32_t sectors; uint32_t base; } cfile_t;
typedef struct { int track, index, is_data, mode2048, file; uint32_t file_sector; uint32_t start, length; } entry_t;

static cfile_t files[MAXF];
static int nfiles;
static entry_t ent[MAXE];
static int nent;

static void die(const char *m, const char *a) { fprintf(stderr, "ngcdconv: %s %s\n", m, a ? a : ""); exit(1); }

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

/* ------------------------------------------------------------ IMA ADPCM */
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int pred, idx; } ima_t;

static int ima_encode(ima_t *s, int sample) {
    int step = ima_step[s->idx], diff = sample - s->pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }
    if (code & 8) s->pred -= vpdiff; else s->pred += vpdiff;
    if (s->pred > 32767) s->pred = 32767;
    if (s->pred < -32768) s->pred = -32768;
    s->idx += ima_index[code];
    if (s->idx < 0) s->idx = 0;
    if (s->idx > 88) s->idx = 88;
    return code;
}

/* ------------------------------------------------------------ CUE */
static uint32_t msf_to_sec(const char *s) {
    int m, sec, f;
    if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3) die("bad time", s);
    return (uint32_t)(m * 4500 + sec * 75 + f);
}

static void parse_cue(const char *cue) {
    FILE *f = fopen(cue, "r");
    if (!f) die("cannot open", cue);
    char dir[1024];
    strncpy(dir, cue, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) slash[1] = 0; else dir[0] = 0;

    char line[2048];
    int cur_track = 0, cur_data = 0, cur_2048 = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) die("bad FILE line", p);
            if (nfiles >= MAXF) die("too many files", NULL);
            cfile_t *cf = &files[nfiles];
            snprintf(cf->path, sizeof cf->path, "%s%.*s", dir, (int)(q2 - q1 - 1), q1 + 1);
            FILE *b = fopen(cf->path, "rb");
            if (!b) die("cannot open", cf->path);
            fseek64(b, 0, SEEK_END);
            long long sz = (long long)ftell64(b);
            fclose(b);
            cf->sectors = (uint32_t)(sz / 2352);
            cf->base = nfiles ? files[nfiles - 1].base + files[nfiles - 1].sectors : 0;
            nfiles++;
        } else if (!strncmp(p, "TRACK", 5)) {
            char type[64];
            if (sscanf(p, "TRACK %d %63s", &cur_track, type) != 2) die("bad TRACK", p);
            cur_data = strncmp(type, "AUDIO", 5) != 0;
            cur_2048 = strstr(type, "/2048") != NULL;
            if (cur_2048) die("MODE1/2048 images are not supported (need 2352-byte sectors)", NULL);
        } else if (!strncmp(p, "INDEX", 5)) {
            int idx; char t[64];
            if (sscanf(p, "INDEX %d %63s", &idx, t) != 2) die("bad INDEX", p);
            if (nent >= MAXE) die("too many entries", NULL);
            entry_t *e = &ent[nent++];
            e->track = cur_track; e->index = idx; e->is_data = cur_data; e->mode2048 = cur_2048;
            e->file = nfiles - 1;
            e->file_sector = msf_to_sec(t);
            e->start = files[nfiles - 1].base + e->file_sector;
        }
    }
    fclose(f);
    uint32_t total = files[nfiles - 1].base + files[nfiles - 1].sectors;
    for (int i = 0; i < nent; i++)
        ent[i].length = (i + 1 < nent ? ent[i + 1].start : total) - ent[i].start;
}

/* ------------------------------------------------------------ main */
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: ngcdconv game.cue outdir\n"); return 1; }
    parse_cue(argv[1]);
    if (make_dir(argv[2]) != 0 && errno != EEXIST) die("cannot create", argv[2]);
    uint32_t total = files[nfiles - 1].base + files[nfiles - 1].sectors;
    char path[1200];

    /* TOC.BIN */
    snprintf(path, sizeof path, "%s/TOC.BIN", argv[2]);
    FILE *o = fopen(path, "wb");
    if (!o) die("cannot write", path);
    int first = 99, last = 0;
    for (int i = 0; i < nent; i++) {
        if (ent[i].track < first) first = ent[i].track;
        if (ent[i].track > last) last = ent[i].track;
    }
    fwrite("NGTOC1\0\0", 1, 8, o);
    put32(o, (uint32_t)nent); put32(o, total);
    fputc(first, o); fputc(last, o); put16(o, 0);
    for (int i = 0; i < nent; i++) {
        fputc(ent[i].track, o); fputc(ent[i].index, o); fputc(ent[i].is_data, o); fputc(0, o);
        put32(o, ent[i].start); put32(o, ent[i].length);
    }
    fclose(o);
    printf("TOC: %d entries, %u sectors, tracks %d-%d\n", nent, total, first, last);

    static uint8_t sec[2352];
    for (int i = 0; i < nent; i++) {
        entry_t *e = &ent[i];
        if (e->index != 1) continue;
        /* the track runs until the next track's first entry */
        uint32_t end = total;
        for (int j = i + 1; j < nent; j++) if (ent[j].track != e->track) { end = ent[j].start; break; }
        uint32_t nsec = end - e->start;
        FILE *in = fopen(files[e->file].path, "rb");
        if (!in) die("cannot open", files[e->file].path);
        fseek64(in, (long long)e->file_sector * 2352, SEEK_SET);
        if (e->is_data) {
            snprintf(path, sizeof path, "%s/DATA.ISO", argv[2]);
            o = fopen(path, "wb");
            if (!o) die("cannot write", path);
            for (uint32_t s = 0; s < nsec; s++) {
                if (fread(sec, 1, 2352, in) != 2352) memset(sec, 0, sizeof sec);
                fwrite(sec + 16, 1, 2048, o);
            }
            fclose(o);
            printf("track %02d: data, %u sectors -> DATA.ISO\n", e->track, nsec);
        } else {
            snprintf(path, sizeof path, "%s/TRK%02d.NGA", argv[2], e->track);
            o = fopen(path, "wb");
            if (!o) die("cannot write", path);
            fwrite("NGA1", 1, 4, o); put32(o, 44100); put16(o, 2); put16(o, 0); put32(o, nsec);
            ima_t st[2] = { { 0, 0 }, { 0, 0 } };
            for (uint32_t s = 0; s < nsec; s++) {
                if (fread(sec, 1, 2352, in) != 2352) memset(sec, 0, sizeof sec);
                for (int c = 0; c < 2; c++) {          /* decoder state for this sector */
                    put16(o, (uint16_t)(int16_t)st[c].pred); fputc(st[c].idx, o); fputc(0, o);
                }
                for (int k = 0; k < 588; k++) {
                    int l = (int16_t)(sec[k * 4] | (sec[k * 4 + 1] << 8));
                    int r = (int16_t)(sec[k * 4 + 2] | (sec[k * 4 + 3] << 8));
                    int cl = ima_encode(&st[0], l), cr = ima_encode(&st[1], r);
                    fputc(cl | (cr << 4), o);
                }
            }
            fclose(o);
            printf("track %02d: audio, %u sectors (%u:%02u) -> TRK%02d.NGA\n", e->track, nsec,
                   nsec / 4500, (nsec / 75) % 60, e->track);
        }
        fclose(in);
    }
    return 0;
}

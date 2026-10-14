/* Neo Geo CD: LC8951 CD-ROM decoder, CD drive command protocol and drive
   position/TOC. After neocd_libretro lc8951.cpp, cdromcontroller.cpp and
   cdrom.cpp. Audio tracks are handed to the host backend (ng.io). */
#include <string.h>
#ifdef NGCD_DEBUG
#include <stdio.h>
#endif
#include "ngcd.h"

enum { CdIdle = 0x00, CdPlaying = 0x10, CdSeeking = 0x20, CdScanning = 0x30,
       CdPaused = 0x40, CdStopped = 0x90, CdEndOfDisc = 0xC0 };

/* LC8951 bits */
#define DECIEN 0x20
#define DOUTEN 0x02
#define DECI   0x20
#define DTEI   0x40
#define DTBSY  0x08
#define DECEN  0x80
#define AUTORQ 0x10
#define MODRQ  0x08
#define FORMRQ 0x04
#define SHDREN 0x01
#define CRCOK  0x80

#define SCAN_SPEED 30

static inline uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline uint32_t unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static inline void msf(uint32_t pos, uint32_t *m, uint32_t *s, uint32_t *f) {
    *m = pos / 4500; *s = (pos / 75) % 60; *f = pos % 75;
}

/* ------------------------------------------------------------ TOC */
static int toc_find_sector(uint32_t sector) {
    int r = -1;
    for (int i = 0; i < ng.toc.n; i++) {
        if (ng.toc.e[i].start <= sector) r = i;
        else break;
    }
    return r < 0 ? 0 : r;
}

static int toc_find_track(uint8_t track, uint8_t index) {
    for (int i = 0; i < ng.toc.n; i++)
        if (ng.toc.e[i].track == track && ng.toc.e[i].index == index) return i;
    return -1;
}

static uint32_t track_position(uint8_t track) {
    int i = toc_find_track(track, 1);
    return i < 0 ? 0 : ng.toc.e[i].start;
}

/* start of INDEX 01 of the track entry i belongs to: a track may be split
   into many indexes (INDEX 02, 03, ... e.g. Samurai Shodown IV has 93 in
   its data track), but data offsets, audio and the relative time all count
   from INDEX 01 */
static uint32_t track_base(int i) {
    const ngcd_toc_entry_t *e = &ng.toc.e[i];
    for (int j = i; j >= 0 && ng.toc.e[j].track == e->track; j--)
        if (ng.toc.e[j].index == 1) return ng.toc.e[j].start;
    return e->start;
}

static int track_is_data(uint8_t track) {
    int i = toc_find_track(track, 1);
    return i >= 0 && ng.toc.e[i].is_data;
}

int ngcd_cd_is_data(void) {
    return ng.cd_track_idx >= 0 && ng.toc.e[ng.cd_track_idx].is_data;
}

int ngcd_cd_is_playing(void) { return ng.cd_playing; }

/* ------------------------------------------------------------ audio follow */
static void audio_sync(void) {
    int want = 0;
    int32_t off = 0;
    if (ng.cd_playing && ng.cd_track_idx >= 0 && ng.cd_pos < ng.toc.total) {
        const ngcd_toc_entry_t *e = &ng.toc.e[ng.cd_track_idx];
        if (!e->is_data && e->index >= 1) {
            want = e->track;
            off = (int32_t)(ng.cd_pos - track_base(ng.cd_track_idx));
        }
    }
    if (want == ng.audio_track_playing) return;
    if (ng.io) {
        if (want) ng.io->audio_play(want, off);
        else ng.io->audio_stop();
    }
    ng.audio_track_playing = want;
}

static void cd_seek(uint32_t pos) {
    ng.cd_pos = pos;
    ng.cd_track_idx = ng.toc.n ? toc_find_sector(pos) : -1;
    /* a seek always restarts the audio stream at the new place */
    if (ng.audio_track_playing && ng.io) ng.io->audio_stop();
    ng.audio_track_playing = 0;
    audio_sync();
}

static void cd_play(void) { ng.cd_playing = 1; audio_sync(); }

void ngcd_cd_stop(void) { ng.cd_playing = 0; audio_sync(); }

static void cd_increase_position(void) {
    if (!ng.cd_playing) return;
    ng.cd_pos++;
    ng.cd_track_idx = toc_find_sector(ng.cd_pos);
    audio_sync();
}

static void cd_read_data(uint8_t *buf) {
    if (!ngcd_cd_is_data() || ng.cd_pos >= ng.toc.total || !ng.io) { memset(buf, 0, 2048); return; }
    if (!ng.io->read_sector(ng.cd_pos - track_base(ng.cd_track_idx), buf)) memset(buf, 0, 2048);
    ng.sectors_read++;
}

/* ------------------------------------------------------------ LC8951 */
static void head_registers(uint32_t lba) {
    uint32_t m, s, f;
    msf(lba + 150, &m, &s, &f);
    ng.HEAD0 = bcd(m); ng.HEAD1 = bcd(s); ng.HEAD2 = bcd(f); ng.HEAD3 = 0x01;
}

static inline uint16_t wreg(uint8_t lo, uint8_t hi) { return (uint16_t)((hi << 8) | lo); }
static inline void set_wreg(uint8_t *lo, uint8_t *hi, uint16_t v) { *hi = (uint8_t)(v >> 8); *lo = (uint8_t)v; }
static inline void add_wreg(uint8_t *lo, uint8_t *hi, uint16_t v) { set_wreg(lo, hi, (uint16_t)(wreg(*lo, *hi) + v)); }

static void inc_regptr(void) {
    if (!ng.lc_regptr) return;
    ng.lc_regptr = (ng.lc_regptr & 0xF0) | ((ng.lc_regptr + 1) & 0x0F);
}

uint8_t lc_read_reg(void) {
    uint8_t v = 0;
    switch (ng.lc_regptr & 0xF) {
    case 0x0: v = ng.COMIN; break;
    case 0x1: v = ng.IFSTAT; break;
    case 0x2: v = ng.DBCL; break;
    case 0x3: v = ng.DBCH; break;
    case 0x4: v = (ng.CTRL1 & SHDREN) ? 0 : ng.HEAD0; break;
    case 0x5: v = (ng.CTRL1 & SHDREN) ? 0 : ng.HEAD1; break;
    case 0x6: v = (ng.CTRL1 & SHDREN) ? 0 : ng.HEAD2; break;
    case 0x7: v = (ng.CTRL1 & SHDREN) ? 0 : ng.HEAD3; break;
    case 0x8: v = ng.PTL; break;
    case 0x9: v = ng.PTH; break;
    case 0xA: v = ng.WAL; break;
    case 0xB: v = ng.WAH; break;
    case 0xC: v = ng.STAT0; break;
    case 0xD: v = ng.STAT1; break;
    case 0xE: v = ng.STAT2; break;
    case 0xF: v = ng.STAT3; ng.IFSTAT &= ~DECI; break;
    }
    inc_regptr();
    return v;
}

void lc_write_reg(uint8_t v) {
    switch (ng.lc_regptr & 0xF) {
    case 0x0: ng.SBOUT = v; break;
    case 0x1: ng.IFCTRL = v; break;
    case 0x2: ng.DBCL = v; break;
    case 0x3: ng.DBCH = v; break;
    case 0x4: ng.DACL = v; break;
    case 0x5: ng.DACH = v; break;
    case 0x6: ng.DTRG = v; if (ng.IFCTRL & DOUTEN) ng.IFSTAT &= ~DTBSY; break;
    case 0x7: ng.DTACK = v; ng.IFSTAT |= DTEI; break;
    case 0x8: ng.WAL = v; break;
    case 0x9: ng.WAH = v; break;
    case 0xA: ng.CTRL0 = v; break;
    case 0xB: ng.CTRL1 = v; break;
    case 0xC: ng.PTL = v; break;
    case 0xD: ng.PTH = v; break;
    case 0xE: break;       /* CTRL2 */
    case 0xF: break;       /* RESET */
    }
    inc_regptr();
}

uint8_t lc_read_response(void) {
    uint8_t b = ng.resp[ng.resp_ptr >> 1];
    if (ng.resp_ptr & 1) return (uint8_t)((b & 0x0F) | (ng.strobe << 4));
    return (uint8_t)((b >> 4) | (ng.strobe << 4));
}

void lc_write_command(uint8_t v) {
    uint8_t *c = &ng.cmd[ng.cmd_ptr >> 1];
    if (ng.cmd_ptr & 1) *c = (uint8_t)((*c & 0xF0) | (v & 0x0F));
    else                *c = (uint8_t)((*c & 0x0F) | (v << 4));
}

static uint8_t packet_checksum(const uint8_t *p) {
    uint8_t c = 0;
    for (int i = 0; i < 4; i++) { c += p[i] >> 4; c += p[i] & 0x0F; }
    c += p[4] >> 4;
    c += 5;
    return (uint8_t)((~c) & 0x0F);
}

static void set_checksum(uint8_t *p) { p[4] = (uint8_t)((p[4] & 0xF0) | packet_checksum(p)); }

static void process_command(void);

void lc_packet_pointer(uint8_t v) {
    switch (v) {
    case 1:
        ng.cmd_ptr = (ng.cmd_ptr + 1) % 10;
        if (!ng.cmd_ptr) process_command();
        break;
    case 2:
        ng.strobe = 0;
        ng.resp_ptr = (ng.resp_ptr + 1) % 10;
        break;
    case 3:
        ng.strobe = 1;
        break;
    }
}

void lc_reset_packet_pointers(void) {
    ng.cmd_ptr = 0;
    ng.resp_ptr = 9;
    ng.strobe = 1;
}

static void lc_sector_decoded(void) {
    if (!(ng.CTRL0 & DECEN)) return;
    head_registers(ng.cd_pos);
    if (!ngcd_cd_is_data()) return;
    cd_read_data(ng.lcbuf);
    add_wreg(&ng.WAL, &ng.WAH, 2352);
    add_wreg(&ng.PTL, &ng.PTH, 2352);
    ng.STAT0 = CRCOK;
    ng.STAT1 = 0;
    ng.STAT2 = (ng.CTRL0 & AUTORQ) ? (ng.CTRL1 & MODRQ) : (ng.CTRL1 & (MODRQ | FORMRQ));
    ng.STAT3 = 0;
    ng.IFSTAT &= ~DECI;
}

void lc_end_transfer(void) {
    ng.IFSTAT |= DTBSY;
    add_wreg(&ng.DACL, &ng.DACH, (uint16_t)(wreg(ng.DBCL, ng.DBCH) + 1));
    set_wreg(&ng.DBCL, &ng.DBCH, 0);
}

/* ------------------------------------------------------------ drive commands */
static void resp_set(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e) {
    ng.resp[0] = a; ng.resp[1] = b; ng.resp[2] = c; ng.resp[3] = d; ng.resp[4] = e;
}

static void process_command(void) {
    const uint8_t *c = ng.cmd;
    uint32_t m, s, f;
    if ((c[4] & 0x0F) != packet_checksum(c)) {
        resp_set((uint8_t)ng.cd_status, 0, 0, 0, 0);
        set_checksum(ng.resp);
        return;
    }
    uint8_t datafl = ngcd_cd_is_data() ? 0x40 : 0x00;
#ifdef NGCD_DEBUG
    if (c[0] != 0x00 && c[0] != 0x20)
        fprintf(stderr, "[cd] cmd %02X %02X %02X %02X %02X pos %u\n", c[0], c[1], c[2], c[3], c[4], ng.cd_pos);
#endif
    switch (c[0]) {
    case 0x00:
        ng.resp[0] = (uint8_t)((ng.resp[0] & 0x0F) | ng.cd_status);
        break;
    case 0x10:
        ngcd_cd_stop();
        ng.cd_status = CdIdle;
        resp_set((uint8_t)ng.cd_status, 0, 0, 0, 0);
        break;
    case 0x20:
        if (ng.cd_status == CdIdle && ng.toc.n) ng.cd_status = CdStopped;
        switch (c[1] & 0x0F) {
        case 0x00:
            msf(ng.cd_pos + 150, &m, &s, &f);
            resp_set((uint8_t)ng.cd_status, bcd(m), bcd(s), bcd(f), datafl);
            break;
        case 0x01: {
            const ngcd_toc_entry_t *e = ng.cd_track_idx >= 0 ? &ng.toc.e[ng.cd_track_idx] : NULL;
            uint32_t p = 0;
            if (e) {
                if (e->index == 0) p = (e->start + e->length) - (ng.cd_pos + 1);
                else p = ng.cd_pos - track_base(ng.cd_track_idx);
            }
            msf(p, &m, &s, &f);
            resp_set((uint8_t)(ng.cd_status | 0x01), bcd(m), bcd(s), bcd(f), datafl);
            break;
        }
        case 0x02: {
            const ngcd_toc_entry_t *e = ng.cd_track_idx >= 0 ? &ng.toc.e[ng.cd_track_idx] : NULL;
            resp_set((uint8_t)(ng.cd_status | 0x02), bcd(e ? e->track : 0), bcd(e ? e->index : 0), 0, datafl);
            break;
        }
        case 0x03:
            msf(ng.toc.total + 150, &m, &s, &f);
            resp_set((uint8_t)(ng.cd_status | 0x03), bcd(m), bcd(s), bcd(f), 0);
            break;
        case 0x04:
            resp_set((uint8_t)(ng.cd_status | 0x04), bcd(ng.toc.first), bcd(ng.toc.last), 0, 0);
            break;
        case 0x05: {
            uint8_t t = (uint8_t)unbcd(c[2]);
            msf(track_position(t) + 150, &m, &s, &f);
            resp_set((uint8_t)(ng.cd_status | 0x05), bcd(m), bcd(s),
                     (uint8_t)(bcd(f) | (track_is_data(t) ? 0x80 : 0)), (uint8_t)(c[2] << 4));
            break;
        }
        case 0x06:
            if (ng.cd_pos >= ng.toc.total) ng.cd_status = CdEndOfDisc;
            resp_set((uint8_t)(ng.cd_status | 0x06), 0, 0, 0, datafl);
            break;
        case 0x07:
            resp_set((uint8_t)(ng.cd_status | 0x07), 0x02, 0, 0, 0);
            break;
        default:
            resp_set((uint8_t)ng.cd_status, 0, 0, 0, 0);
            break;
        }
        break;
    case 0x30: {
        uint32_t pos = unbcd(c[1]) * 4500 + unbcd(c[2]) * 75 + unbcd(c[3]);
        pos = pos >= 150 ? pos - 150 : 0;
        ng.cd_playing = 1;
        cd_seek(pos);
        ng.cd_status = CdPlaying;
        const ngcd_toc_entry_t *e = ng.cd_track_idx >= 0 ? &ng.toc.e[ng.cd_track_idx] : NULL;
        resp_set((uint8_t)(ng.cd_status | 0x02), bcd(e ? e->track : 0), 0, 0, 0);
        break;
    }
    case 0x40:
        ngcd_cd_stop();
        ng.cd_status = CdPaused;
        resp_set(CdSeeking, 0, 0, 0, 0);
        break;
    case 0x50:
        ng.resp[0] = (uint8_t)ng.cd_status;
        break;
    case 0x60:
        ngcd_cd_stop();
        ng.cd_status = CdPaused;
        ng.resp[0] = (uint8_t)ng.cd_status;
        break;
    case 0x70:
        cd_play();
        ng.cd_status = CdPlaying;
        ng.resp[0] = (uint8_t)ng.cd_status;
        break;
    case 0x80: {
        uint32_t p = ng.cd_pos + SCAN_SPEED;
        if (p > ng.toc.total - 1) p = ng.toc.total - 1;
        cd_seek(p);
        ng.cd_status = CdPlaying;
        ng.resp[0] = CdScanning;
        break;
    }
    case 0x90: {
        uint32_t p = ng.cd_pos < SCAN_SPEED ? 0 : ng.cd_pos - SCAN_SPEED;
        cd_seek(p);
        ng.cd_status = CdPlaying;
        ng.resp[0] = CdScanning;
        break;
    }
    case 0xB0: {
        uint8_t t = (uint8_t)unbcd(c[1]);
        ng.cd_playing = 1;
        cd_seek(track_position(t));
        ng.cd_status = CdPlaying;
        const ngcd_toc_entry_t *e = ng.cd_track_idx >= 0 ? &ng.toc.e[ng.cd_track_idx] : NULL;
        resp_set((uint8_t)(ng.cd_status | 0x02), bcd(e ? e->track : 0), 0, 0, 0);
        break;
    }
    default:
        resp_set((uint8_t)ng.cd_status, 0, 0, 0, 0);
        break;
    }
    set_checksum(ng.resp);
}

/* ------------------------------------------------------------ timer */
#define CD_64HZ  ((int32_t)(NGCD_MASTER_CLOCK / 64.64 + 0.5))
#define CD_75HZ  ((int32_t)(NGCD_MASTER_CLOCK / 75.0 + 0.5))
#define CD_150HZ ((int32_t)(NGCD_MASTER_CLOCK / 150.0 + 0.5))

void ngcd_cd_tick(void) {
    int playing = ng.cd_playing, data = ngcd_cd_is_data();
    int32_t delay;
    if (playing) {
        delay = data ? (ng.is_cdz ? CD_150HZ : CD_75HZ) : CD_75HZ;
        if (data && ng.cd_speed_mul > 1) delay /= ng.cd_speed_mul;
    } else {
        delay = CD_64HZ;
    }
    ngcd_timer_arm_rel(T_CDROM, delay);

    if (playing) {
        lc_sector_decoded();
        if ((ng.CTRL0 & DECEN) && (ng.irq_mask1 & 0x500) == 0x500 &&
            (ng.IFCTRL & DECIEN) && !(ng.IFSTAT & DECI))
            ng.pending_irq |= IRQ_CDDEC;
        cd_increase_position();
    }
    if ((ng.irq_mask1 & 0x50) == 0x50 && ng.cd_comm_nreset)
        ng.pending_irq |= IRQ_CDCOMM;
    ngcd_update_irq();
}

void ngcd_cd_reset(void) {
    memset(ng.cmd, 0, sizeof(ng.cmd));
    memset(ng.resp, 0, sizeof(ng.resp));
    set_checksum(ng.resp);
    ng.lc_regptr = 0;
    lc_reset_packet_pointers();
    head_registers(0);
    ng.SBOUT = ng.IFCTRL = ng.DBCL = ng.DBCH = ng.DACL = ng.DACH = ng.DTRG = ng.DTACK = 0;
    ng.WAL = 0x30; ng.WAH = 0x09;
    ng.CTRL0 = ng.CTRL1 = ng.PTL = ng.PTH = ng.COMIN = 0;
    ng.IFSTAT = 0xFF;
    ng.STAT0 = ng.STAT1 = ng.STAT2 = ng.STAT3 = 0;
    memset(ng.lcbuf, 0, sizeof(ng.lcbuf));
    ng.cd_status = CdIdle;
    ng.cd_playing = 0;
    ng.audio_track_playing = 0;
    if (ng.io && ng.io->audio_stop) ng.io->audio_stop();
    ng.cd_pos = 0;
    ng.cd_track_idx = ng.toc.n ? toc_find_sector(0) : -1;
}

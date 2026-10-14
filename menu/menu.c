/* ngcd-dc anthology menu.

   Reads /cd/MENU.TXT, one game per line:

       SSI|Samurai Spirits
       SSII|Samurai Spirits II
       SSIII|Samurai Spirits III|NGCD.BIN

   An optional line "@Header text" replaces the header (default SAMURAI
   SHODOWN ANTHOLOGY); lines starting with # are comments.

   folder | title | emulator binary (optional, default NGCD.BIN; a plain,
   unscrambled binary in the disc's root). The chosen binary is loaded,
   the game folder ("/cd/SSII/") is written after the NGCD-DISC-DIR=
   marker inside the image, and the image is started with arch_exec().
   There is no way back from a game (reset the console).

   Drawn with the BIOS font on a 640x480 screen. */
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_QUIET);

#define MAX_GAMES  16
#define MARKER     "NGCD-DISC-DIR="
#define MARKER_ROOM 50                    /* bytes after the marker (64 - 14) */

#define C_TITLE    0xFD20                 /* orange */
#define C_SEL      0xFFFF
#define C_ITEM     0x7BEF                 /* grey */
#define C_ERR      0xF800

typedef struct { char dir[40]; char title[64]; char bin[40]; } game_t;

static game_t games[MAX_GAMES];
static int ngames;
static char status[96];
static char header[64] = "SAMURAI SHODOWN ANTHOLOGY";

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void load_list(void) {
    char line[200];
    FILE *f = fopen("/cd/MENU.TXT", "r");
    if (!f) return;
    while (ngames < MAX_GAMES && fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        if (*l == '@') { snprintf(header, sizeof(header), "%s", trim(l + 1)); continue; }
        char *t = strchr(l, '|');
        if (!t) continue;
        *t++ = 0;
        char *b = strchr(t, '|');
        if (b) *b++ = 0;
        game_t *g = &games[ngames];
        char *dir = trim(l);
        while (*dir == '/') dir++;
        snprintf(g->dir, sizeof(g->dir), "%s", dir);
        snprintf(g->title, sizeof(g->title), "%s", trim(t));
        snprintf(g->bin, sizeof(g->bin), "%s", b && *trim(b) ? trim(b) : "NGCD.BIN");
        if (g->dir[0]) ngames++;
    }
    fclose(f);
}

static void text(int x, int y, const char *s, uint16_t fg) {
    bfont_set_foreground_color(fg);
    bfont_set_background_color(0);
    bfont_draw_str(vram_s + y * 640 + x, 640, 1, s);
}

static void draw(int sel) {
    memset(vram_s, 0, 640 * 480 * 2);
    int hx = 320 - (int)strlen(header) * 6;      /* BIOS font: 12 px wide */
    text(hx < 16 ? 16 : hx, 60, header, C_TITLE);
    for (int i = 0; i < ngames; i++)
        text(140, 140 + i * 32, games[i].title, i == sel ? C_SEL : C_ITEM);
    if (ngames)
        text(116, 140 + sel * 32, ">", C_TITLE);
    else
        text(140, 140, "MENU.TXT not found or empty", C_ERR);
    if (status[0])
        text(140, 380, status, C_ERR);
    text(278, 430, "A: play", C_ITEM);
}

static void *find(void *buf, size_t len, const char *pat) {
    size_t n = strlen(pat);
    for (uint8_t *p = buf, *e = (uint8_t *)buf + len - n; p <= e; p++)
        if (*p == (uint8_t)pat[0] && !memcmp(p, pat, n)) return p;
    return NULL;
}

static void launch(int i) {
    const game_t *g = &games[i];
    char path[96], dir[64];
    snprintf(path, sizeof(path), "/cd/%s", g->bin);
    snprintf(dir, sizeof(dir), "/cd/%s/", g->dir);
    status[0] = 0;
    if (strlen(dir) >= MARKER_ROOM) { snprintf(status, sizeof(status), "folder name too long"); return; }

    file_t f = fs_open(path, O_RDONLY);
    if (f == FILEHND_INVALID) { snprintf(status, sizeof(status), "%s not found", path); return; }
    size_t len = fs_total(f);
    void *buf = malloc(len);
    int ok = buf && fs_read(f, buf, len) == (ssize_t)len;
    fs_close(f);
    if (!ok) { free(buf); snprintf(status, sizeof(status), "cannot read %s", path); return; }

    char *m = find(buf, len, MARKER);
    if (!m) { free(buf); snprintf(status, sizeof(status), "%s: no folder marker", g->bin); return; }
    m += strlen(MARKER);
    memset(m, 0, MARKER_ROOM);
    memcpy(m, dir, strlen(dir));

    memset(vram_s, 0, 640 * 480 * 2);
    text(140, 220, "Loading...", C_ITEM);
    arch_exec(buf, len);                 /* does not return */
}

int main(void) {
    int sel = 0;
    uint32_t prev = ~0u;                 /* ignore buttons held at power-on */

    vid_set_mode(DM_640x480, PM_RGB565);
    load_list();
    draw(sel);

    for (;;) {
        uint32_t b = 0;
        MAPLE_FOREACH_BEGIN(MAPLE_FUNC_CONTROLLER, cont_state_t, st)
            b |= st->buttons;
            if (st->joyy < -64) b |= CONT_DPAD_UP;
            if (st->joyy > 64)  b |= CONT_DPAD_DOWN;
        MAPLE_FOREACH_END()
        uint32_t pressed = b & ~prev;
        prev = b;
        if (ngames) {
            if (pressed & CONT_DPAD_UP)   { sel = (sel + ngames - 1) % ngames; draw(sel); }
            if (pressed & CONT_DPAD_DOWN) { sel = (sel + 1) % ngames; draw(sel); }
            if (pressed & (CONT_A | CONT_START)) { launch(sel); draw(sel); }
        }
        thd_sleep(16);
    }
    return 0;
}

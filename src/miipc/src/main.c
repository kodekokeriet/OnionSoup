/* MiiPC - a sample-based groovebox for Miyoo Mini / Mini Plus / Mini v4 (Onion OS). */
#include "app.h"
#include "help.h"
#include "project.h"
#include "settings.h"
#include "undo.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

App A;
static int running = 1;

/* Small set of currently held keys (SDL2 keycodes are too large for a table). */
static SDLKey held[16];
int key_held(SDLKey k)
{
    for (int i = 0; i < 16; i++)
        if (held[i] == k && k)
            return 1;
    return 0;
}
static void key_set(SDLKey k, int on)
{
    for (int i = 0; i < 16; i++)
        if (held[i] == k) {
            if (!on)
                held[i] = 0;
            return;
        }
    if (on)
        for (int i = 0; i < 16; i++)
            if (!held[i]) {
                held[i] = k;
                return;
            }
}

/* Named buttons for the test script (MIIPC_SCRIPT). */
static SDLKey script_key(const char *t)
{
    static const struct {
        const char *n;
        SDLKey k;
    } map[] = {
        {"up", BTN_UP},
        {"down", BTN_DOWN},
        {"left", BTN_LEFT},
        {"right", BTN_RIGHT},
        {"a", BTN_A},
        {"b", BTN_B},
        {"x", BTN_X},
        {"y", BTN_Y},
        {"l1", BTN_L1},
        {"r1", BTN_R1},
        {"l2", BTN_L2},
        {"r2", BTN_R2},
        {"select", BTN_SELECT},
        {"start", BTN_START},
        {"menu", BTN_MENU},
#ifndef MIYOO
        {"help", SDLK_F1},
#endif
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(t, map[i].n))
            return map[i].k;
    return 0;
}

void app_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toast_until = SDL_GetTicks() + 2500;
}

Color color_mix(Color a, Color b, float t)
{
    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
    int bl = (a & 255) + (int)((((int)b & 255) - (int)(a & 255)) * t);
    return (Color)(r << 16 | g << 8 | bl);
}

/* One "LABEL   value" row; highlighted when selected. */
void draw_field(int x, int y, int w, int selected, const char *label, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (selected)
        gfx_rect(x, y - 3, w, 18, C_SEL);
    gfx_text(x + 6, y, 1, selected ? C_ACCENT : C_DIM, "%s", label);
    int vx = x + 6 + (w < 300 ? 92 : 116);
    int maxc = (x + w - vx - 4) / 8;
    if ((int)strlen(buf) > maxc && maxc > 0)
        buf[maxc] = 0;
    gfx_text(vx, y, 1, C_TEXT, "%s", buf);
}

static const char *project_title(void)
{
    static char buf[64];
    if (!A.project_path[0])
        return "unsaved project";
    const char *b = strrchr(A.project_path, '/');
    snprintf(buf, sizeof buf, "%s", b ? b + 1 : A.project_path);
    char *dot = strrchr(buf, '.');
    if (dot)
        *dot = 0;
    return buf;
}

/* ---------- file browser ---------- */

void open_sample_browser(void)
{
    A.br_purpose = BR_SAMPLE;
    char title[64];
    snprintf(title, sizeof title, "LOAD SAMPLE TO PAD %d", A.cur_pad + 1);
    browser_open(&A.br, A.sample_dir[0] ? A.sample_dir : "samples", ".wav", title);
}

static void close_browser(void)
{
    engine_stop_preview(&A.e);
    sample_free(&A.preview);
    browser_close(&A.br);
    A.br_purpose = BR_NONE;
}

static void browser_input(SDLKey k)
{
    char path[1024];
    if (k == BTN_X && A.br_purpose == BR_SAMPLE) {
        if (browser_current_file(&A.br, path, sizeof path)) {
            engine_stop_preview(&A.e);
            sample_free(&A.preview);
            if (sample_load_wav(&A.preview, path) == 0)
                engine_preview(&A.e, &A.preview, 0, A.preview.frames);
            else
                app_toast("Could not read file (PCM WAV only)");
        }
        return;
    }
    int r = browser_key(&A.br, k, path, sizeof path);
    if (r < 0) {
        close_browser();
        return;
    }
    if (r == 0)
        return;
    if (A.br_purpose == BR_SAMPLE) {
        snprintf(A.sample_dir, sizeof A.sample_dir, "%s", A.br.dir);
        int idx = engine_load_sample(&A.e, path);
        if (idx < 0) {
            app_toast("Could not read file (PCM WAV only)");
            return;
        }
        undo_mark(&A.e, NULL);
        engine_assign(&A.e, A.cur_pad, idx);
        app_toast("Loaded %s to pad %d", A.e.pool[idx].name, A.cur_pad + 1);
        close_browser();
    }
    else if (A.br_purpose == BR_PROJECT) {
        close_browser();
        if (project_load(&A.e, path, NULL, 0) == 0) {
            snprintf(A.project_path, sizeof A.project_path, "%s", path);
            undo_clear();
            app_toast("Opened %s", project_title());
        }
        else
            app_toast("Could not open project");
    }
}

/* ---------- project menu (MENU) ---------- */

enum { M_RESUME,
       M_HELP,
       M_SAVE,
       M_SAVE_NEW,
       M_OPEN,
       M_NEW,
       M_SEQ_COPY,
       M_SEQ_CLEAR,
       M_RESAMPLE,
       M_EXPORT,
       M_QUIT,
       NUM_MENU };
static const char *menu_items[NUM_MENU] = {
    "Resume",
    "Help / controls",
    "Save",
    "Save as new project",
    "Open project",
    "New project",
    "Copy sequence to next empty",
    "Clear sequence",
    "Resample sequence to a pad",
    "Export sequence as WAV",
    "Quit",
};

static void draw(void);

/* Render the current sequence offline into dir/prefix_NNN.wav. */
static int render_to_file(const char *dir, const char *prefix, float tail, char *path, int len)
{
    int16_t *buf = NULL;
    int frames = engine_render(&A.e, A.e.cur_seq, tail, &buf);
    if (frames <= 0) {
        free(buf);
        return -1;
    }
    mkdir(dir, 0755);
    for (int n = 1; n < 1000; n++) {
        snprintf(path, len, "%s/%s_%03d.wav", dir, prefix, n);
        if (access(path, F_OK) != 0)
            break;
    }
    int r = sample_save_wav(path, buf, (uint32_t)frames, 2, A.e.out_rate);
    free(buf);
    return r;
}

static void busy(const char *msg)
{
    snprintf(A.toast, sizeof A.toast, "%s", msg);
    A.toast_until = SDL_GetTicks() + 60000;
    A.menu_open = 0;
    draw();
}

static void do_resample(void)
{
    if (!A.e.seqs[A.e.cur_seq].nev) {
        app_toast("Sequence %02d is empty", A.e.cur_seq + 1);
        return;
    }
    busy("Resampling...");
    char path[1024];
    if (mkdir("samples", 0755), render_to_file("samples/resampled", "resample", 1.0f, path, sizeof path) != 0) {
        app_toast("Resample failed (SD card full?)");
        return;
    }
    int idx = engine_load_sample(&A.e, path);
    if (idx < 0) {
        app_toast("Resample failed");
        return;
    }
    int pad = -1;
    for (int i = 0; i < NUM_PADS && pad < 0; i++)
        if (A.e.pads[i].sample < 0)
            pad = i;
    if (pad < 0)
        pad = A.cur_pad;
    undo_mark(&A.e, NULL);
    engine_assign(&A.e, pad, idx);
    A.cur_pad = pad;
    A.bank = pad / PADS_PER_BANK;
    app_toast("Resampled seq %02d to pad %d", A.e.cur_seq + 1, pad + 1);
}

static void do_export(void)
{
    if (!A.e.seqs[A.e.cur_seq].nev) {
        app_toast("Sequence %02d is empty", A.e.cur_seq + 1);
        return;
    }
    busy("Exporting...");
    char path[1024];
    if (render_to_file("exports", "export", 2.0f, path, sizeof path) != 0) {
        app_toast("Export failed (SD card full?)");
        return;
    }
    app_toast("Saved %s", path);
}

static void do_save(int as_new)
{
    if (as_new || !A.project_path[0])
        project_next_path(A.project_path, sizeof A.project_path);
    if (project_save(&A.e, A.project_path, NULL) == 0) {
        app_toast("Saved %s", project_title());
        undo_saved();
    }
    else
        app_toast("Save failed!");
}

static void menu_input(SDLKey k)
{
    if (k == BTN_UP) {
        A.menu_sel = (A.menu_sel + NUM_MENU - 1) % NUM_MENU;
        A.menu_confirm = 0;
    }
    if (k == BTN_DOWN) {
        A.menu_sel = (A.menu_sel + 1) % NUM_MENU;
        A.menu_confirm = 0;
    }
    if (k == BTN_B || k == BTN_MENU || k == BTN_START) {
        A.menu_open = 0;
        return;
    }
    if (k != BTN_A)
        return;
    switch (A.menu_sel) {
    case M_RESUME:
        A.menu_open = 0;
        break;
    case M_HELP:
        A.menu_open = 0;
        A.help_open = 1;
        break;
    case M_SAVE:
        do_save(0);
        A.menu_open = 0;
        break;
    case M_SAVE_NEW:
        do_save(1);
        A.menu_open = 0;
        break;
    case M_OPEN:
        A.menu_open = 0;
        mkdir(PROJECT_DIR, 0755);
        A.br_purpose = BR_PROJECT;
        browser_open(&A.br, PROJECT_DIR, ".mpj", "OPEN PROJECT");
        break;
    case M_NEW:
        if (!A.menu_confirm) {
            A.menu_confirm = 1;
            break;
        }
        engine_reset(&A.e);
        undo_clear();
        A.project_path[0] = 0;
        A.menu_open = 0;
        A.menu_confirm = 0;
        app_toast("New empty project");
        break;
    case M_SEQ_COPY: {
        int dst = -1;
        for (int i = 1; i < MAX_SEQS && dst < 0; i++) {
            int c = (A.e.cur_seq + i) % MAX_SEQS;
            if (A.e.seqs[c].nev == 0)
                dst = c;
        }
        if (dst < 0)
            app_toast("No empty sequence left");
        else {
            seq_copy(&A.e, A.e.cur_seq, dst);
            engine_select_seq(&A.e, dst);
            app_toast("Copied sequence %02d to %02d", A.e.cur_seq + 1, dst + 1);
        }
        A.menu_open = 0;
        break;
    }
    case M_SEQ_CLEAR:
        if (!A.menu_confirm) {
            A.menu_confirm = 1;
            break;
        }
        undo_mark(&A.e, NULL);
        seq_remove_range(&A.e, A.e.cur_seq, -1, 0, 0xffffffffu);
        A.menu_open = 0;
        A.menu_confirm = 0;
        app_toast("Sequence %02d cleared", A.e.cur_seq + 1);
        break;
    case M_RESAMPLE:
        do_resample();
        break;
    case M_EXPORT:
        do_export();
        break;
    case M_QUIT:
        running = 0;
        break;
    }
}

static void draw_menu(void)
{
    int ih = SCREEN_H >= 540 ? 26 : 22;
    int w = 340, h = 60 + NUM_MENU * ih, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx_rect(x + 6, y + 6, w, h, 0x000000);
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_frame(x, y, w, h, 2, C_ACCENT);
    gfx_text(x + 14, y + 10, 1, C_DIM, "PROJECT");
    gfx_text(x + 14, y + 26, 1, C_TEXT, "%s", project_title());
    for (int i = 0; i < NUM_MENU; i++) {
        int ry = y + 52 + i * ih;
        if (i == A.menu_sel)
            gfx_rect(x + 6, ry - 5, w - 12, ih - 2, C_SEL);
        const char *t = menu_items[i];
        if (A.menu_confirm && i == A.menu_sel && i == M_NEW)
            t = "Press A again: discard all";
        if (A.menu_confirm && i == A.menu_sel && i == M_SEQ_CLEAR)
            t = "Press A again: clear it";
        gfx_text(x + 18, ry, 1, i == A.menu_sel ? C_ACCENT : C_TEXT, "%s", t);
    }
}

/* ---------- frame around the views ---------- */

/* Battery percent for the header: Onion writes /tmp/percBat, laptops have
 * /sys/class/power_supply. Read every 10 s. */
static void update_status(void)
{
    static Uint32 next;
    Uint32 now = SDL_GetTicks();
    if (now < next)
        return;
    next = now + 10000;
    int pct = -1;
    FILE *f = fopen("/tmp/percBat", "r");
    if (f) {
        if (fscanf(f, "%d", &pct) != 1)
            pct = -1;
        fclose(f);
    }
    if (pct < 0) {
        DIR *d = opendir("/sys/class/power_supply");
        struct dirent *de;
        while (d && pct < 0 && (de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            char p[512];
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", de->d_name);
            if ((f = fopen(p, "r"))) {
                if (fscanf(f, "%d", &pct) != 1)
                    pct = -1;
                fclose(f);
            }
        }
        if (d)
            closedir(d);
    }
    if (pct >= 0 && pct <= 100)
        snprintf(A.status, sizeof A.status, "BAT %d%%", pct);
    else
        A.status[0] = 0;
}

static void draw_header(void)
{
    gfx_rect(0, 0, SCREEN_W, HEADER_H, C_PANEL);
    gfx_text(10, 7, 2, C_ACCENT, "MiiPC");
    static const char *tabs[NUM_VIEWS] = {"PLAY", "SEQ", "MIX", "SAMPLE", "SONG"};
    int tx = 96;
    for (int i = 0; i < NUM_VIEWS; i++) {
        int tw = gfx_text_width(tabs[i], 1);
        if (i == A.view)
            gfx_rect(tx - 5, 6, tw + 10, 20, C_SEL);
        gfx_text(tx, 10, 1, i == A.view ? C_ACCENT : C_DIM, "%s", tabs[i]);
        tx += tw + 14;
    }
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    int bx = SCREEN_W - 250;
    gfx_text(bx, 7, 2, C_TEXT, "%3d", A.e.bpm);
    gfx_text(bx + 52, 14, 1, C_DIM, "BPM");
    double t = A.e.playing ? A.e.play_tick : 0;
    if (t < 0)
        gfx_text(bx + 86, 14, 1, C_REC, "S%02d IN %d", A.e.cur_seq + 1, (int)(-t / PPQ) + 1);
    else
        gfx_text(bx + 86, 14, 1, C_DIM, "S%02d %2d.%d", A.e.cur_seq + 1, (int)(t / (4 * PPQ)) + 1, (int)(t / PPQ) % 4 + 1);
    gfx_text(bx + 172, 14, 1, A.e.playing ? C_PLAY : C_DIM, A.e.playing ? "PLAY" : "STOP");
    if (A.e.rec)
        gfx_text(bx + 212, 14, 1, C_REC, "REC");
    else if (A.e.song_mode)
        gfx_text(bx + 212, 14, 1, C_ACCENT, "SONG");
    /* right of the position bar: CPU load and battery */
    update_status();
    char st[48];
    snprintf(st, sizeof st, "CPU %2d%%%s%s", (int)(A.e.cpu_load * 100 + 0.5f), A.status[0] ? "  " : "", A.status);
    int sw = gfx_text_width(st, 1);
    gfx_text(SCREEN_W - sw - 10, HEADER_H - 13, 1, A.e.cpu_load > 0.8f ? C_REC : C_DIM, "%s", st);
    /* sequence position: one segment per bar, playhead fills */
    int bw = SCREEN_W - 20 - sw - 16;
    double frac = A.e.playing && t > 0 ? t / seq_len(sq) : 0;
    for (int b = 0; b < sq->bars; b++) {
        int x0 = 10 + b * bw / sq->bars, x1 = 10 + (b + 1) * bw / sq->bars - 3;
        if (x1 <= x0)
            x1 = x0 + 1;
        gfx_rect(x0, HEADER_H - 8, x1 - x0, 4, b % 4 == 0 ? C_DIM : C_PAD);
    }
    if (A.e.playing && t >= 0)
        gfx_rect(10 + (int)(frac * bw), HEADER_H - 10, 3, 8, C_PLAY);
}

static void draw_footer(void)
{
    int y = SCREEN_H - FOOTER_H;
    gfx_rect(0, y, SCREEN_W, FOOTER_H, C_PANEL);
    if (SDL_GetTicks() < A.toast_until)
        gfx_text(10, y + 5, 1, C_ACCENT, "%s", A.toast);
    else {
        const char *h = A.settings_open ? settings_help() : A.menu_open         ? "{UD} choose  A select  B close"
                                                        : A.view == VIEW_PLAY   ? view_play_help()
                                                        : A.view == VIEW_SEQ    ? view_seq_help()
                                                        : A.view == VIEW_MIX    ? view_mix_help()
                                                        : A.view == VIEW_SAMPLE ? view_sample_help()
                                                                                : view_song_help();
        char buf[256];
        keys_expand(h, buf, sizeof buf);
        gfx_text(10, y + 5, 1, C_DIM, "%s", buf);
        /* help hint, right-aligned, only if it fits */
        char hint[32];
        keys_expand("{HELP}: help", hint, sizeof hint);
        int hw = gfx_text_width(hint, 1);
        if (gfx_text_width(buf, 1) + hw + 30 < SCREEN_W)
            gfx_text(SCREEN_W - hw - 10, y + 5, 1, C_ACCENT, "%s", hint);
    }
}

static void draw(void)
{
    gfx_clear(C_BG);
    draw_header();
    int top = HEADER_H + 6, bottom = SCREEN_H - FOOTER_H - 6;
    if (A.view == VIEW_PLAY)
        view_play_draw(top, bottom);
    else if (A.view == VIEW_SEQ)
        view_seq_draw(top, bottom);
    else if (A.view == VIEW_MIX)
        view_mix_draw(top, bottom);
    else if (A.view == VIEW_SAMPLE)
        view_sample_draw(top, bottom);
    else
        view_song_draw(top, bottom);
    if (A.br.active) {
        int m = SCREEN_W / 16;
        browser_draw(&A.br, m, HEADER_H + 4, SCREEN_W - 2 * m, SCREEN_H - HEADER_H - FOOTER_H - 8);
    }
    if (A.settings_open)
        settings_draw();
    if (A.menu_open)
        draw_menu();
    if (A.help_open)
        help_draw(A.view);
    draw_footer();
    gfx_present();
}

static void handle_key(SDLKey k, int rep)
{
#ifdef KEY_HELP
    if (k == KEY_HELP || k == SDLK_F1) {
        if (!rep)
            A.help_open = !A.help_open;
        return;
    }
#endif
    if (A.help_open) {
        if (!rep && k != BTN_SHIFT)
            A.help_open = 0;
        return;
    } /* any key closes help */
    if (A.br.active) {
        browser_input(k);
        return;
    }
    if (A.menu_open) {
        if (!rep || k == BTN_UP || k == BTN_DOWN)
            menu_input(k);
        return;
    }
    if (A.settings_open) {
        settings_key(k, rep);
        return;
    }
    if (k == BTN_SHIFT)
        return; /* R2 is only a modifier */
    int shift = key_held(BTN_SHIFT);
    if ((k == BTN_MENU || k == BTN_START) && !rep) {
        A.menu_open = 1;
        A.menu_sel = 0;
        A.menu_confirm = 0;
        return;
    }
    if (k == BTN_SELECT && !rep) {
        settings_open();
        return;
    }
    if ((k == BTN_L1 || k == BTN_R1) && shift) {
        int ok = k == BTN_L1 ? undo_undo(&A.e) : undo_redo(&A.e);
        if (ok)
            app_toast("%s  %d/%d", k == BTN_L1 ? "Undo" : "Redo", undo_pos(), undo_count());
        else
            app_toast(k == BTN_L1 ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    if ((k == BTN_L1 || k == BTN_R1) && !rep) {
        A.view = (A.view + (k == BTN_R1 ? 1 : NUM_VIEWS - 1)) % NUM_VIEWS;
        return;
    }
    if (k == BTN_L2 && !rep) {
        if (shift) {
            if (A.e.rec) {
                A.e.rec = 0;
                app_toast("Recording off (still playing)");
            }
            else {
                undo_mark(&A.e, NULL);
                engine_start_recording(&A.e);
                app_toast(A.e.play_tick < 0 ? "Count-in... then recording" : "Recording");
            }
        }
        else {
            if (A.e.playing) {
                engine_set_playing(&A.e, 0);
                A.e.rec = 0;
            }
            else
                engine_set_playing(&A.e, 1);
        }
        return;
    }
    if (A.view == VIEW_PLAY)
        view_play_key(k, rep, shift);
    else if (A.view == VIEW_SEQ)
        view_seq_key(k, rep, shift);
    else if (A.view == VIEW_MIX)
        view_mix_key(k, rep, shift);
    else if (A.view == VIEW_SAMPLE)
        view_sample_key(k, rep, shift);
    else
        view_song_key(k, rep, shift);
}

static void handle_keyup(SDLKey k)
{
    key_set(k, 0);
    if (A.view == VIEW_PLAY)
        view_play_keyup(k);
}

/* First start: load a kit from a folder and write a small demo beat. */
static void load_default_kit(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    char *names[64];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 64) {
        const char *ext = strrchr(de->d_name, '.');
        if (ext && !strcasecmp(ext, ".wav"))
            names[n++] = strdup(de->d_name);
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j], names[j - 1]) < 0; j--) {
            char *t = names[j];
            names[j] = names[j - 1];
            names[j - 1] = t;
        }
    char base[4096];
    if (!realpath(dir, base))
        snprintf(base, sizeof base, "%s", dir);
    for (int i = 0; i < n; i++) {
        if (i < NUM_PADS) {
            char path[4200];
            snprintf(path, sizeof path, "%s/%s", base, names[i]);
            int idx = engine_load_sample(&A.e, path);
            if (idx >= 0)
                engine_assign(&A.e, i, idx);
            if (strstr(names[i], "hat"))
                A.e.pads[i].choke = 1;
        }
        free(names[i]);
    }
    /* two-bar demo groove in sequence 1 (16th = PPQ/4 ticks) */
    const int q = PPQ / 4;
    for (int bar = 0; bar < 2; bar++) {
        int o = bar * 16;
        seq_add(&A.e, 0, (o + 0) * q, 0, 120);
        seq_add(&A.e, 0, (o + (bar ? 3 : 10)) * q, 0, 100);
        if (bar)
            seq_add(&A.e, 0, (o + 10) * q, 0, 110);
        seq_add(&A.e, 0, (o + 4) * q, 2, 110);
        seq_add(&A.e, 0, (o + 12) * q, 2, 110);
        for (int s = 0; s < 16; s += 2)
            seq_add(&A.e, 0, (o + s) * q, 6, s % 4 ? 70 : 100);
    }
    A.e.pads[2].send_rev = 0.25f;
}

int main(int argc, char **argv)
{
    const char *kit = argc > 1 ? argv[1] : "samples/kit01";
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (gfx_init() < 0) {
        fprintf(stderr, "video: %s\n", SDL_GetError());
        return 1;
    }
#if SDL_MAJOR_VERSION < 2
    SDL_EnableKeyRepeat(320, 45);
#endif

    if (engine_init(&A.e) < 0)
        fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());

    /* Restore the previous session, otherwise start with the default kit. */
    char origin[512] = "";
    if (!getenv("MIIPC_FRESH") && project_load(&A.e, AUTOSAVE, origin, sizeof origin) == 0) {
        if (origin[0] && access(origin, F_OK) == 0)
            snprintf(A.project_path, sizeof A.project_path, "%s", origin);
    }
    else {
        load_default_kit(kit);
        if (!getenv("MIIPC_SCRIPT"))
            A.help_open = 1; /* first start: show the controls */
    }

    const char *script = getenv("MIIPC_SCRIPT"); /* test helper, see below */
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
            else if (ev.type == SDL_KEYDOWN) {
                SDLKey k = ev.key.keysym.sym;
#ifndef MIYOO
                if (k == SDLK_SPACE)
                    k = BTN_L2; /* extra PC aliases */
                if (k == SDLK_RSHIFT || k == SDLK_LSHIFT)
                    k = BTN_R2;
#endif
#if SDL_MAJOR_VERSION >= 2
                int rep = ev.key.repeat || key_held(k);
#else
                int rep = key_held(k);
#endif
                key_set(k, 1);
                handle_key(k, rep);
            }
            else if (ev.type == SDL_KEYUP) {
                SDLKey k = ev.key.keysym.sym;
#ifndef MIYOO
                if (k == SDLK_SPACE)
                    k = BTN_L2;
                if (k == SDLK_RSHIFT || k == SDLK_LSHIFT)
                    k = BTN_R2;
#endif
                handle_keyup(k);
            }
        }
        Uint32 now = SDL_GetTicks();
        float decay = (now - last) / 1000.0f * 6.0f;
        last = now;
        for (int i = 0; i < NUM_PADS; i++) {
            A.e.pad_flash[i] -= decay;
            if (A.e.pad_flash[i] < 0)
                A.e.pad_flash[i] = 0;
            float m = A.e.pad_meter[i];
            A.e.pad_meter[i] = 0; /* peak since last frame */
            A.meter[i] = m > A.meter[i] ? m : A.meter[i] * 0.85f;
        }
        for (int c = 0; c < 2; c++) {
            float m = A.e.master_meter[c];
            A.e.master_meter[c] = 0;
            A.mmeter[c] = m > A.mmeter[c] ? m : A.mmeter[c] * 0.85f;
        }

        if (script) {
            /* one step per frame: button name, h<btn>/u<btn> hold/release, w<ms> wait,
             * "s:file" screenshot, "q" quit */
            static char buf[2048];
            static char *pos = NULL;
            if (!pos) {
                snprintf(buf, sizeof buf, "%s", script);
                pos = buf;
            }
            char *tok = pos;
            char *c = strchr(pos, ',');
            if (c) {
                *c = 0;
                pos = c + 1;
            }
            else
                pos = buf + strlen(buf);
            if (!*tok)
                running = 0;
            else if (tok[0] == 's' && tok[1] == ':') {
                draw();
                gfx_save(tok + 2);
            }
            else if (tok[0] == 'w' && tok[1] >= '0' && tok[1] <= '9')
                SDL_Delay(atoi(tok + 1));
            else if (!strcmp(tok, "q"))
                running = 0;
            else if (tok[0] == 'h' && script_key(tok + 1)) {
                key_set(script_key(tok + 1), 1);
                handle_key(script_key(tok + 1), 0);
            }
            else if (tok[0] == 'u' && script_key(tok + 1))
                handle_keyup(script_key(tok + 1));
            else if (script_key(tok))
                handle_key(script_key(tok), 0);
        }
        static Uint32 next_autosave;
        if (!next_autosave)
            next_autosave = now + 120000;
        if (now >= next_autosave) {
            next_autosave = now + 120000;
            if (undo_dirty() && project_save(&A.e, AUTOSAVE, A.project_path) == 0)
                undo_saved();
        }
        draw();
        SDL_Delay(16);
    }

    project_save(&A.e, AUTOSAVE, A.project_path);
    if (A.br.active)
        close_browser();
    engine_quit(&A.e);
    gfx_quit();
    SDL_Quit();
    return 0;
}

/* SAMPLE view: waveform, region start/end, voice parameters and chopping. */
#include "app.h"
#include "help.h"
#include "undo.h"
#include <stdio.h>
#include <string.h>

enum { F_START,
       F_END,
       F_PITCH,
       F_ATTACK,
       F_DECAY,
       F_CHOKE,
       F_CHOP,
       NUM_FIELDS };

enum { CHOP_EQUAL,
       CHOP_TRANSIENT,
       CHOP_TAP };
typedef struct {
    const char *name;
    int n;
    int kind;
} ChopMode;
static const ChopMode chop_modes[] = {
    {"tap: A plays, X marks", 16, CHOP_TAP},
    {"2 equal", 2, 0},
    {"4 equal", 4, 0},
    {"8 equal", 8, 0},
    {"16 equal", 16, 0},
    {"transients, max 4", 4, 1},
    {"transients, max 8", 8, 1},
    {"transients, max 16", 16, 1},
};
#define NUM_CHOP (int)(sizeof chop_modes / sizeof chop_modes[0])
#define ZOOM_FPP 16 /* frames per pixel in the zoom strip */

/* Cache chop points so transient detection does not run every frame. */
static struct {
    const Sample *s;
    uint32_t start, end;
    int mode, ntap, snap;
    int n;
    uint32_t pts[16];
} chop_cache;

static int chop_points(Sample *s, Pad *p, uint32_t *pts)
{
    if (chop_cache.s == s && chop_cache.start == p->start && chop_cache.end == p->end && chop_cache.mode == A.chop_mode &&
        chop_cache.ntap == A.ntap && chop_cache.snap == A.snap) {
        memcpy(pts, chop_cache.pts, sizeof chop_cache.pts);
        return chop_cache.n;
    }
    const ChopMode *m = &chop_modes[A.chop_mode];
    int n;
    if (m->kind == CHOP_TRANSIENT)
        n = sample_detect_onsets(s, p->start, p->end, pts, m->n);
    else if (m->kind == CHOP_TAP) {
        n = 1;
        pts[0] = p->start;
        for (int i = 0; i < A.ntap && n < 16; i++)
            if (A.tap_marks[i] > p->start + 64 && A.tap_marks[i] < p->end - 64)
                pts[n++] = A.tap_marks[i];
        for (int i = 1; i < n; i++)
            for (int j = i; j > 1 && pts[j] < pts[j - 1]; j--) {
                uint32_t t = pts[j];
                pts[j] = pts[j - 1];
                pts[j - 1] = t;
            }
    }
    else {
        n = m->n;
        for (int i = 0; i < n; i++)
            pts[i] = p->start + (uint64_t)(p->end - p->start) * i / n;
    }
    if (A.snap)
        for (int i = 1; i < n; i++)
            pts[i] = sample_zero_cross(s, pts[i], 512);
    chop_cache.s = s;
    chop_cache.start = p->start;
    chop_cache.end = p->end;
    chop_cache.mode = A.chop_mode;
    chop_cache.n = n;
    chop_cache.ntap = A.ntap;
    chop_cache.snap = A.snap;
    memcpy(chop_cache.pts, pts, sizeof chop_cache.pts);
    return n;
}

static int x_of(uint32_t frame, uint32_t frames, int x, int w)
{
    return x + (int)((uint64_t)frame * (w - 1) / (frames ? frames : 1));
}

static void draw_overview(Sample *s, Pad *p, int x, int y, int w, int h)
{
    gfx_rect(x, y, w, h, C_BG);
    int xs = x_of(p->start, s->frames, x, w), xe = x_of(p->end, s->frames, x, w);
    gfx_rect(xs, y, xe - xs + 1, h, C_REGION);
    const int16_t *pk = sample_peaks(s, w);
    int mid = y + h / 2;
    for (int i = 0; i < w && pk; i++) {
        int y0 = mid - pk[i * 2 + 1] * (h / 2 - 2) / 32768;
        int y1 = mid - pk[i * 2] * (h / 2 - 2) / 32768;
        gfx_vline(x + i, y0, y1, (x + i >= xs && x + i <= xe) ? C_WAVE : C_DIM);
    }
    uint32_t pts[16];
    int n = chop_points(s, p, pts);
    for (int i = 1; i < n; i++)
        gfx_vline(x_of(pts[i], s->frames, x, w), y + 4, y + h - 5, C_PLAY);
    double pp = engine_preview_pos();
    if (pp >= 0)
        gfx_rect(x_of((uint32_t)pp, s->frames, x, w), y, 2, h, C_PLAY);
    gfx_rect(xs, y, 2, h, A.smp_field == F_START ? C_CURSOR : C_ACCENT);
    gfx_rect(xe - 1, y, 2, h, A.smp_field == F_END ? C_CURSOR : C_ACCENT);
    gfx_frame(x - 1, y - 1, w + 2, h + 2, 1, C_PANEL2);
}

/* Magnified view around the marker being edited, for precise cuts. */
static void draw_zoom(Sample *s, Pad *p, int x, int y, int w, int h)
{
    gfx_rect(x, y, w, h, C_BG);
    uint32_t m = A.smp_field == F_END ? p->end : p->start;
    int64_t first = (int64_t)m - (int64_t)(w / 2) * ZOOM_FPP;
    int mid = y + h / 2;
    for (int i = 0; i < w; i++) {
        int64_t a = first + (int64_t)i * ZOOM_FPP;
        if (a < 0 || a >= s->frames)
            continue;
        int mn = 32767, mx = -32768;
        for (int64_t f = a; f < a + ZOOM_FPP && f < s->frames; f++) {
            int v = s->channels == 2 ? (s->data[f * 2] + s->data[f * 2 + 1]) / 2 : s->data[f];
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        int in = a >= p->start && a < p->end;
        if (in)
            gfx_vline(x + i, y, y + h - 1, C_REGION);
        gfx_vline(x + i, mid - mx * (h / 2 - 2) / 32768, mid - mn * (h / 2 - 2) / 32768, in ? C_WAVE : C_DIM);
    }
    gfx_vline(x + w / 2, y, y + h - 1, C_CURSOR);
    gfx_frame(x - 1, y - 1, w + 2, h + 2, 1, C_PANEL2);
    gfx_text(x + 4, y + 2, 1, C_DIM, "ZOOM %s", A.smp_field == F_END ? "END" : "START");
}

static void fmt_time(char *b, int len, uint32_t frames, int rate)
{
    snprintf(b, len, "%.3f s", rate ? (double)frames / rate : 0.0);
}

void view_sample_draw(int top, int bottom)
{
    Pad *p = &A.e.pads[A.cur_pad];
    Sample *s = engine_pad_sample(&A.e, A.cur_pad);
    int x = 12, w = SCREEN_W - 24, y = top + 4;

    gfx_text(x, y, 1, C_DIM, "PAD %d (%c%d)", A.cur_pad + 1, 'A' + A.cur_pad / 4, A.cur_pad % 4 + 1);
    gfx_text(x + 110, y, 1, C_TEXT, "%.40s", p->label[0] ? p->label : "(empty)");
    if (s)
        gfx_text(SCREEN_W - 12 - gfx_text_width("44100 Hz stereo", 1), y, 1, C_DIM, "%d Hz %s", s->rate, s->channels == 2 ? "stereo" : "mono");
    y += 18;

    int fields_h = NUM_FIELDS * 18 + 6;
    int wave_total = bottom - y - fields_h - 12;
    if (!s) {
        gfx_rect(x, y, w, wave_total, C_PANEL);
        const char *t1 = "Empty pad", *t2 = "Press Y to load a WAV file from the SD card.";
        gfx_text(x + (w - gfx_text_width(t1, 2)) / 2, y + wave_total / 2 - 24, 2, C_TEXT, "%s", t1);
        gfx_text(x + (w - gfx_text_width(t2, 1)) / 2, y + wave_total / 2 + 8, 1, C_DIM, "%s", t2);
        return;
    }
    int ov_h = wave_total * 62 / 100, zm_h = wave_total - ov_h - 8;
    draw_overview(s, p, x, y, w, ov_h);
    draw_zoom(s, p, x, y + ov_h + 8, w, zm_h);
    y += wave_total + 10;

    uint32_t pts[16];
    int nchop = chop_points(s, p, pts);
    char t[32];
    int fw = w + 8, fx = x - 4;
    fmt_time(t, sizeof t, p->start, s->rate);
    draw_field(fx, y, fw, A.smp_field == F_START, "START", "%s", t);
    y += 18;
    fmt_time(t, sizeof t, p->end, s->rate);
    draw_field(fx, y, fw, A.smp_field == F_END, "END", "%s   (length %u ms)", t,
               (unsigned)((uint64_t)(p->end - p->start) * 1000 / s->rate));
    y += 18;
    draw_field(fx, y, fw, A.smp_field == F_PITCH, "PITCH", "%+d semitones", p->pitch);
    y += 18;
    draw_field(fx, y, fw, A.smp_field == F_ATTACK, "ATTACK", "%d ms", (int)p->attack);
    y += 18;
    if (p->decay > 0.5f)
        draw_field(fx, y, fw, A.smp_field == F_DECAY, "DECAY", "%d ms", (int)p->decay);
    else
        draw_field(fx, y, fw, A.smp_field == F_DECAY, "DECAY", "off (play to end)");
    y += 18;
    if (p->choke)
        draw_field(fx, y, fw, A.smp_field == F_CHOKE, "CHOKE", "group %d", p->choke);
    else
        draw_field(fx, y, fw, A.smp_field == F_CHOKE, "CHOKE", "off");
    y += 18;
    draw_field(fx, y, fw, A.smp_field == F_CHOP, "CHOP", "< %s >  %s %d-%d",
               chop_modes[A.chop_mode].name, K("{SH}+A: to pads"), A.cur_pad + 1, (A.cur_pad + nchop - 1) % NUM_PADS + 1);
}

static void apply_chop(void)
{
    Pad *src = &A.e.pads[A.cur_pad];
    Sample *s = engine_pad_sample(&A.e, A.cur_pad);
    if (!s)
        return;
    uint32_t pts[16];
    int n = chop_points(s, src, pts);
    undo_mark(&A.e, NULL);
    Pad base = *src;
    char name[24];
    snprintf(name, sizeof name, "%s", s->name);
    name[16] = 0;
    engine_lock();
    for (int i = 0; i < n; i++) {
        Pad *d = &A.e.pads[(A.cur_pad + i) % NUM_PADS];
        *d = base;
        d->start = pts[i];
        d->end = i + 1 < n ? pts[i + 1] : base.end;
        snprintf(d->label, sizeof d->label, "%s %d", name, i + 1);
    }
    engine_unlock();
    engine_gc(&A.e); /* overwritten pads may have released a sample */
    app_toast("Chopped into %d slices on pads %d-%d", n, A.cur_pad + 1, (A.cur_pad + n - 1) % NUM_PADS + 1);
}

void view_sample_key(SDLKey k, int rep, int shift)
{
    Pad *p = &A.e.pads[A.cur_pad];
    Sample *s = engine_pad_sample(&A.e, A.cur_pad);

    if (shift) {
        if (k == BTN_LEFT || k == BTN_RIGHT) {
            A.cur_pad = (A.cur_pad + (k == BTN_RIGHT ? 1 : NUM_PADS - 1)) % NUM_PADS;
            A.bank = A.cur_pad / PADS_PER_BANK;
            A.ntap = 0;
        }
        if (rep)
            return;
        if (k == BTN_A && s)
            apply_chop();
        if (k == BTN_Y) {
            A.clip = *p;
            A.clip_valid = 1;
            app_toast("Copied pad %d", A.cur_pad + 1);
        }
        if (k == BTN_B && A.clip_valid) {
            if (A.clip.sample >= 0 && !A.e.pool[A.clip.sample].data) {
                app_toast("The copied sample is no longer loaded");
                return;
            }
            undo_mark(&A.e, NULL);
            engine_lock();
            *p = A.clip;
            engine_unlock();
            engine_gc(&A.e);
            app_toast("Pasted onto pad %d", A.cur_pad + 1);
        }
        return;
    }
    if (k == BTN_Y && !rep) {
        open_sample_browser();
        return;
    }
    if (!s)
        return;
    if (k == BTN_UP)
        A.smp_field = (A.smp_field + NUM_FIELDS - 1) % NUM_FIELDS;
    if (k == BTN_DOWN)
        A.smp_field = (A.smp_field + 1) % NUM_FIELDS;
    int tapmode = A.smp_field == F_CHOP && chop_modes[A.chop_mode].kind == CHOP_TAP;
    if (k == BTN_A && !rep) {
        if (tapmode) {
            A.ntap = 0;
            engine_preview(&A.e, s, p->start, p->end);
        }
        else
            engine_trigger(&A.e, A.cur_pad, 110);
    }
    if (k == BTN_X && !rep && tapmode) {
        double pos = engine_preview_pos();
        if (pos < 0)
            app_toast("Press A first, then X to mark while it plays");
        else if (A.ntap < 15)
            A.tap_marks[A.ntap++] = (uint32_t)pos;
    }
    if (k == BTN_B && !rep) {
        if (tapmode && A.ntap) {
            A.ntap--;
            app_toast("Removed last mark");
        }
        else
            engine_preview(&A.e, s, 0, s->frames);
    }

    int dir = k == BTN_RIGHT ? 1 : k == BTN_LEFT ? -1
                                                 : 0;
    if (!dir)
        return;
    /* Coarse: 1/200 of the file. Hold X for fine: one zoom-strip pixel. */
    int64_t stepf = key_held(BTN_X) ? ZOOM_FPP : (s->frames / 200 > 1 ? s->frames / 200 : 1);
    if (A.smp_field != F_CHOP)
        undo_mark(&A.e, A.smp_field == F_START ? "s0" : A.smp_field == F_END ? "s1"
                                                                             : "s2");
    engine_lock();
    switch (A.smp_field) {
    case F_START: {
        int64_t v = (int64_t)p->start + dir * stepf;
        if (v < 0)
            v = 0;
        if (v > (int64_t)p->end - 64)
            v = (int64_t)p->end - 64;
        if (v < 0)
            v = 0;
        p->start = (uint32_t)v;
        if (A.snap && !key_held(BTN_X)) {
            uint32_t z = sample_zero_cross(s, p->start, 256);
            if (z + 64 < p->end)
                p->start = z;
        }
        break;
    }
    case F_END: {
        int64_t v = (int64_t)p->end + dir * stepf;
        if (v > s->frames)
            v = s->frames;
        if (v < (int64_t)p->start + 64)
            v = p->start + 64;
        p->end = (uint32_t)v;
        if (A.snap && !key_held(BTN_X)) {
            uint32_t z = sample_zero_cross(s, p->end, 256);
            if (z > p->start + 64)
                p->end = z;
        }
        break;
    }
    case F_PITCH:
        p->pitch += dir;
        if (p->pitch < -12)
            p->pitch = -12;
        if (p->pitch > 12)
            p->pitch = 12;
        break;
    case F_ATTACK:
        p->attack += dir * (p->attack < 20 ? 1 : 10);
        if (p->attack < 0)
            p->attack = 0;
        if (p->attack > 1000)
            p->attack = 1000;
        break;
    case F_DECAY:
        p->decay += dir * (p->decay < 100 ? 10 : 50);
        if (p->decay < 0)
            p->decay = 0;
        if (p->decay > 4000)
            p->decay = 4000;
        break;
    case F_CHOKE:
        p->choke = (p->choke + dir + 5) % 5;
        break;
    case F_CHOP:
        if (!rep)
            A.chop_mode = (A.chop_mode + dir + NUM_CHOP) % NUM_CHOP;
        break;
    }
    engine_unlock();
}

const char *view_sample_help(void)
{
    if (A.smp_field == F_CHOP && chop_modes[A.chop_mode].kind == CHOP_TAP)
        return "A play  X mark  B undo mark  {SH}+A chop to pads";
    return "Y load  A/B hear  {DPAD} edit  {SH}+A chop  {SH}+L/R pad";
}

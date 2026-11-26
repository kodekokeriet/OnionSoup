/* MIX view: 16 channel strips + FX/master strip. L1/R1 picks a channel,
 * the panel below edits it. */
#include "app.h"
#include "undo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { CF_LEVEL, CF_PAN, CF_FILTER, CF_CUTOFF, CF_RESO, CF_DRIVE, CF_CRUSH, CF_DLY, CF_REV, NUM_CF };
enum { XF_DTIME, XF_DFB, XF_DLVL, XF_RSIZE, XF_RDAMP, XF_RLVL, XF_CTHR, XF_CRAT, XF_MASTER, NUM_XF };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void draw_meter(int x, int y, int w, int h, float v)
{
    gfx_rect(x, y, w, h, C_BG);
    float db = v > 0.0001f ? 20.0f * log10f(v) : -60.0f;   /* -48..0 dB scale */
    float f = clampf((db + 48.0f) / 48.0f, 0, 1);
    int mh = (int)(f * h);
    Color c = db > -1.0f ? C_REC : db > -9.0f ? C_PLAY : C_STEP;
    gfx_rect(x, y + h - mh, w, mh, c);
}

static void draw_strip(int ch, int x, int y, int w, int h)
{
    int sel = ch == A.mix_ch;
    gfx_rect(x, y, w, h, sel ? C_SEL : C_PANEL);
    if (sel) gfx_frame(x, y, w, h, 2, C_ACCENT);
    if (ch == MIX_FX) {
        gfx_text(x + (w - 16) / 2, y + 4, 1, sel ? C_ACCENT : C_TEXT, "FX");
        int mh = h - 44;
        draw_meter(x + w / 2 - 7, y + 22, 6, mh, A.mmeter[0]);
        draw_meter(x + w / 2 + 1, y + 22, 6, mh, A.mmeter[1]);
        gfx_text(x + 4, y + h - 16, 1, C_DIM, "MST");
        return;
    }
    Pad *p = &A.e.pads[ch];
    gfx_text(x + (w - (ch >= 9 ? 16 : 8)) / 2, y + 4, 1, sel ? C_ACCENT : (p->sample >= 0 ? C_TEXT : C_DIM), "%d", ch + 1);
    int mh = h - 60, my = y + 22;
    draw_meter(x + 4, my, 5, mh, A.meter[ch]);
    /* fader track + cap */
    int fx = x + w / 2 + 1;
    gfx_rect(fx, my, 2, mh, C_PAD);
    int cap = my + mh - (int)(p->level * mh);
    gfx_rect(fx - 5, cap - 2, 12, 5, sel ? C_CURSOR : C_TEXT);
    /* pan dot */
    int py = y + h - 34;
    gfx_rect(x + 4, py, w - 8, 2, C_PAD);
    gfx_rect(x + 4 + (int)((p->pan + 1) * 0.5f * (w - 12)), py - 2, 4, 6, C_TEXT);
    /* mute / solo */
    int bw = (w - 10) / 2;
    gfx_rect(x + 4, y + h - 24, bw, 18, p->mute ? C_REC : C_PANEL2);
    gfx_text(x + 4 + (bw - 8) / 2, y + h - 21, 1, p->mute ? C_BG : C_DIM, "M");
    gfx_rect(x + 6 + bw, y + h - 24, bw, 18, p->solo ? C_PLAY : C_PANEL2);
    gfx_text(x + 6 + bw + (bw - 8) / 2, y + h - 21, 1, p->solo ? C_BG : C_DIM, "S");
    if (p->filter || p->drive > 0 || p->crush) gfx_rect(x + w - 8, y + 6, 4, 4, C_WAVE); /* insert active */
}

static void fmt_pct(char *b, int n, float v) { snprintf(b, n, "%d", (int)(v * 100 + 0.5f)); }

void view_mix_draw(int top, int bottom)
{
    int n = NUM_PADS + 1;
    int sw = (SCREEN_W - 16) / n, sx = (SCREEN_W - sw * n) / 2;
    int sh = (bottom - top) * 52 / 100;
    for (int c = 0; c < n; c++) draw_strip(c, sx + c * sw + 1, top, sw - 2, sh);

    int y = top + sh + 8, colw = (SCREEN_W - 28) / 2;
    char v[12][40];
    const char *lbl[12];
    int nf;
    if (A.mix_ch == MIX_FX) {
        FxParams *x = &A.e.fx;
        gfx_text(12, y, 1, C_ACCENT, "FX + MASTER");
        nf = NUM_XF;
        lbl[XF_DTIME] = "DELAY TIME";  snprintf(v[XF_DTIME], 40, "%s", dly_sync_names[x->dly_sync]);
        lbl[XF_DFB] = "DELAY FDBK";    fmt_pct(v[XF_DFB], 40, x->dly_fb);
        lbl[XF_DLVL] = "DELAY LEVEL";  fmt_pct(v[XF_DLVL], 40, x->dly_level);
        lbl[XF_RSIZE] = "REVERB SIZE"; fmt_pct(v[XF_RSIZE], 40, x->rev_size);
        lbl[XF_RDAMP] = "REVERB DAMP"; fmt_pct(v[XF_RDAMP], 40, x->rev_damp);
        lbl[XF_RLVL] = "REVERB LEVEL"; fmt_pct(v[XF_RLVL], 40, x->rev_level);
        lbl[XF_CTHR] = "COMP THRESH";
        if (x->comp_thresh > -0.5f) snprintf(v[XF_CTHR], 40, "off"); else snprintf(v[XF_CTHR], 40, "%d dB", (int)x->comp_thresh);
        lbl[XF_CRAT] = "COMP RATIO";   snprintf(v[XF_CRAT], 40, "%.1f:1", x->comp_ratio);
        lbl[XF_MASTER] = "MASTER";     fmt_pct(v[XF_MASTER], 40, x->master);
    } else {
        Pad *p = &A.e.pads[A.mix_ch];
        gfx_text(12, y, 1, C_ACCENT, "CHANNEL %d", A.mix_ch + 1);
        gfx_text(100, y, 1, C_TEXT, "%.30s", p->label[0] ? p->label : "(empty)");
        nf = NUM_CF;
        lbl[CF_LEVEL] = "LEVEL";   fmt_pct(v[CF_LEVEL], 40, p->level);
        lbl[CF_PAN] = "PAN";
        if (fabsf(p->pan) < 0.01f) snprintf(v[CF_PAN], 40, "center");
        else snprintf(v[CF_PAN], 40, "%s %d", p->pan < 0 ? "L" : "R", (int)(fabsf(p->pan) * 100 + 0.5f));
        lbl[CF_FILTER] = "FILTER"; snprintf(v[CF_FILTER], 40, "%s", filter_names[p->filter]);
        lbl[CF_CUTOFF] = "CUTOFF";
        float hz = 20.0f * powf(1000.0f, p->cutoff);
        if (hz >= 1000) snprintf(v[CF_CUTOFF], 40, "%.1f kHz", hz / 1000); else snprintf(v[CF_CUTOFF], 40, "%d Hz", (int)hz);
        lbl[CF_RESO] = "RESONANCE"; fmt_pct(v[CF_RESO], 40, p->reso);
        lbl[CF_DRIVE] = "DRIVE";    fmt_pct(v[CF_DRIVE], 40, p->drive);
        lbl[CF_CRUSH] = "BITCRUSH";
        if (crush_bits[p->crush]) snprintf(v[CF_CRUSH], 40, "%d-bit", crush_bits[p->crush]); else snprintf(v[CF_CRUSH], 40, "off");
        lbl[CF_DLY] = "DELAY SEND";  fmt_pct(v[CF_DLY], 40, p->send_dly);
        lbl[CF_REV] = "REVERB SEND"; fmt_pct(v[CF_REV], 40, p->send_rev);
    }
    y += 18;
    int rows = (nf + 1) / 2;
    for (int i = 0; i < nf; i++) {
        int col = i / rows, row = i % rows;
        draw_field(10 + col * (colw + 8), y + row * 18, colw, A.mix_field == i, lbl[i], "%s", v[i]);
    }
}

static const char *mix_tags[2][12] = {
    { "m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8", "m9", "m10", "m11" },
    { "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11" },
};

static void adjust(int dir, int rep)
{
    undo_mark(&A.e, mix_tags[A.mix_ch == MIX_FX][A.mix_field]);
    engine_lock();
    if (A.mix_ch == MIX_FX) {
        FxParams *x = &A.e.fx;
        switch (A.mix_field) {
        case XF_DTIME:  if (!rep) x->dly_sync = (x->dly_sync + dir + NUM_DLY_SYNC) % NUM_DLY_SYNC; break;
        case XF_DFB:    x->dly_fb = clampf(x->dly_fb + dir * 0.05f, 0, 0.95f); break;
        case XF_DLVL:   x->dly_level = clampf(x->dly_level + dir * 0.05f, 0, 1); break;
        case XF_RSIZE:  x->rev_size = clampf(x->rev_size + dir * 0.05f, 0, 1); break;
        case XF_RDAMP:  x->rev_damp = clampf(x->rev_damp + dir * 0.05f, 0, 1); break;
        case XF_RLVL:   x->rev_level = clampf(x->rev_level + dir * 0.05f, 0, 1); break;
        case XF_CTHR:   x->comp_thresh = clampf(x->comp_thresh + dir * 2.0f, -40, 0); break;
        case XF_CRAT:   x->comp_ratio = clampf(x->comp_ratio + dir * 0.5f, 1, 20); break;
        case XF_MASTER: x->master = clampf(x->master + dir * 0.05f, 0, 1.5f); break;
        }
    } else {
        Pad *p = &A.e.pads[A.mix_ch];
        switch (A.mix_field) {
        case CF_LEVEL:  p->level = clampf(p->level + dir * 0.05f, 0, 1); break;
        case CF_PAN:    p->pan = clampf(p->pan + dir * 0.1f, -1, 1); if (fabsf(p->pan) < 0.01f) p->pan = 0; break;
        case CF_FILTER: if (!rep) p->filter = (p->filter + dir + NUM_FILT) % NUM_FILT; break;
        case CF_CUTOFF: p->cutoff = clampf(p->cutoff + dir * 0.02f, 0, 1); break;
        case CF_RESO:   p->reso = clampf(p->reso + dir * 0.05f, 0, 1); break;
        case CF_DRIVE:  p->drive = clampf(p->drive + dir * 0.05f, 0, 1); break;
        case CF_CRUSH:  if (!rep) p->crush = (p->crush + dir + NUM_CRUSH) % NUM_CRUSH; break;
        case CF_DLY:    p->send_dly = clampf(p->send_dly + dir * 0.05f, 0, 1); break;
        case CF_REV:    p->send_rev = clampf(p->send_rev + dir * 0.05f, 0, 1); break;
        }
    }
    engine_unlock();
}

void view_mix_key(SDLKey k, int rep, int shift)
{
    int nf = A.mix_ch == MIX_FX ? NUM_XF : NUM_CF;
    if (shift && (k == BTN_LEFT || k == BTN_RIGHT)) {
        A.mix_ch = (A.mix_ch + (k == BTN_RIGHT ? 1 : NUM_PADS)) % (NUM_PADS + 1);
        if (A.mix_ch < NUM_PADS) { A.cur_pad = A.mix_ch; A.bank = A.cur_pad / PADS_PER_BANK; }
        int nn = A.mix_ch == MIX_FX ? NUM_XF : NUM_CF;
        if (A.mix_field >= nn) A.mix_field = nn - 1;
        return;
    }
    if (k == BTN_UP)    A.mix_field = (A.mix_field + nf - 1) % nf;
    if (k == BTN_DOWN)  A.mix_field = (A.mix_field + 1) % nf;
    if (k == BTN_LEFT)  adjust(-1, rep);
    if (k == BTN_RIGHT) adjust(1, rep);
    if (rep || A.mix_ch == MIX_FX) return;
    Pad *p = &A.e.pads[A.mix_ch];
    if (k == BTN_A) engine_trigger(&A.e, A.mix_ch, 110);
    if (k == BTN_X) { undo_mark(&A.e, NULL); engine_lock(); p->mute = !p->mute; engine_unlock(); }
    if (k == BTN_Y) { undo_mark(&A.e, NULL); engine_lock(); p->solo = !p->solo; engine_unlock(); }
    if (k == BTN_B && shift) { /* reset channel inserts */
        undo_mark(&A.e, NULL);
        engine_lock(); p->filter = 0; p->cutoff = 1; p->reso = 0; p->drive = 0; p->crush = 0; engine_unlock();
        app_toast("Channel %d effects reset", A.mix_ch + 1);
    }
}

const char *view_mix_help(void)
{
    return A.mix_ch == MIX_FX ? "{SH}+Left/Right channel  {UD} field  {LR} value"
                              : "{SH}+Left/Right chan  A hear  X mute  Y solo";
}

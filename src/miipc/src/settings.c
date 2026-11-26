#include "settings.h"
#include "app.h"
#include "help.h"
#include "undo.h"
#include <stdio.h>

enum { S_TEMPO, S_TAP, S_SWING, S_SEQ, S_BARS, S_QUANT, S_CLICK, S_COUNTIN, S_SNAP, NUM_S };
static int sel;
static Uint32 taps[4];
static int ntaps;

void settings_open(void) { A.settings_open = 1; ntaps = 0; }

static void tap(void)
{
    Uint32 now = SDL_GetTicks();
    if (ntaps && now - taps[(ntaps - 1) % 4] > 2000) ntaps = 0;   /* restart after a pause */
    taps[ntaps % 4] = now;
    ntaps++;
    int n = ntaps < 4 ? ntaps : 4;
    if (n >= 2) {
        Uint32 first = taps[(ntaps - n) % 4], last = taps[(ntaps - 1) % 4];
        float beat = (last - first) / (float)(n - 1);
        int bpm = (int)(60000.0f / beat + 0.5f);
        if (bpm >= 40 && bpm <= 240) { undo_mark(&A.e, "tempo"); A.e.bpm = bpm; }
    }
}

void settings_draw(void)
{
    int w = SCREEN_W * 3 / 4, h = 70 + NUM_S * 20, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    gfx_rect(x + 6, y + 6, w, h, 0x000000);
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_frame(x, y, w, h, 2, C_ACCENT);
    gfx_text(x + 14, y + 10, 2, C_ACCENT, "SETTINGS");
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    int fy = y + 44, fw = w - 16, fx = x + 8;
    char seqv[32];
    if (A.e.next_seq != A.e.cur_seq) snprintf(seqv, sizeof seqv, "%02d  (next: %02d)", A.e.cur_seq + 1, A.e.next_seq + 1);
    else snprintf(seqv, sizeof seqv, "%02d  (%d events)", A.e.cur_seq + 1, sq->nev);
    draw_field(fx, fy, fw, sel == S_TEMPO, "TEMPO", "%d BPM", A.e.bpm); fy += 20;
    draw_field(fx, fy, fw, sel == S_TAP, "TAP TEMPO", "press A in time%s", ntaps >= 2 ? "  (tapping)" : ""); fy += 20;
    draw_field(fx, fy, fw, sel == S_SWING, "SWING", "%d%%", 50 + (int)(A.e.swing * 50 + 0.5f)); fy += 20;
    draw_field(fx, fy, fw, sel == S_SEQ, "SEQUENCE", "%s", seqv); fy += 20;
    draw_field(fx, fy, fw, sel == S_BARS, "BARS", "%d", sq->bars); fy += 20;
    draw_field(fx, fy, fw, sel == S_QUANT, "QUANTIZE", "%s", quant_names[sq->quant]); fy += 20;
    draw_field(fx, fy, fw, sel == S_CLICK, "CLICK", "%s", click_names[A.e.click]); fy += 20;
    draw_field(fx, fy, fw, sel == S_COUNTIN, "COUNT-IN", "%s", A.e.count_in ? "1 bar" : "off"); fy += 20;
    draw_field(fx, fy, fw, sel == S_SNAP, "SNAP CHOPS", "%s", A.snap ? "to zero crossings" : "off"); fy += 20;
}

void settings_key(SDLKey k, int rep)
{
    if ((k == BTN_SELECT || k == BTN_B || k == BTN_START || k == BTN_MENU) && !rep) { A.settings_open = 0; return; }
    if (k == BTN_UP)   sel = (sel + NUM_S - 1) % NUM_S;
    if (k == BTN_DOWN) sel = (sel + 1) % NUM_S;
    if (k == BTN_A && sel == S_TAP && !rep) tap();
    int dir = k == BTN_RIGHT ? 1 : k == BTN_LEFT ? -1 : 0;
    if (!dir) return;
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    switch (sel) {
    case S_TEMPO: undo_mark(&A.e, "tempo"); A.e.bpm += dir; if (A.e.bpm < 40) A.e.bpm = 40; if (A.e.bpm > 240) A.e.bpm = 240; break;
    case S_SWING:
        undo_mark(&A.e, "swing");
        engine_lock();
        A.e.swing += dir * 0.02f;
        if (A.e.swing < 0.001f) A.e.swing = 0;
        if (A.e.swing > 0.5f) A.e.swing = 0.5f;
        engine_unlock();
        break;
    case S_SEQ:   engine_select_seq(&A.e, (A.e.next_seq + dir + MAX_SEQS) % MAX_SEQS); break;
    case S_BARS:  undo_mark(&A.e, "bars"); seq_set_bars(&A.e, A.e.cur_seq, sq->bars + dir); break;
    case S_QUANT: if (!rep) sq->quant = (sq->quant + dir + NUM_QUANT) % NUM_QUANT; break;
    case S_CLICK: if (!rep) A.e.click = (A.e.click + dir + NUM_CLICK) % NUM_CLICK; break;
    case S_COUNTIN: if (!rep) A.e.count_in = !A.e.count_in; break;
    case S_SNAP:  if (!rep) A.snap = !A.snap; break;
    }
}

const char *settings_help(void) { return "{UD} choose  {LR} change  A tap tempo  {SEL}/B close"; }

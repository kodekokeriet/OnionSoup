/* SEQ view: piano-roll style grid editor for the linear sequence.
 * Events are drawn at their exact tick, so live-recorded (unquantized)
 * timing is visible; the cursor snaps to the grid. */
#include "app.h"
#include "undo.h"
#include <stdio.h>
#include <string.h>

static const int zoom_bars[] = {1, 2, 4};
#define NUM_ZOOM 3

static int grid_ticks(void)
{
    int q = quant_ticks[A.e.seqs[A.e.cur_seq].quant];
    return q ? q : PPQ / 4;
}

void view_seq_draw(int top, int bottom)
{
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    uint32_t len = seq_len(sq);
    if (A.grid_tick >= len)
        A.grid_tick = 0;
    int g = grid_ticks();
    uint32_t vis = (uint32_t)zoom_bars[A.grid_zoom] * 4 * PPQ;
    if (vis > len)
        vis = len;
    uint32_t v0 = A.grid_tick / vis * vis;

    const int namew = 96;
    int gx = namew, gw = SCREEN_W - namew - 8;
    /* info line + ruler */
    int bar = A.grid_tick / (4 * PPQ), beat = A.grid_tick / PPQ % 4, tk = A.grid_tick % PPQ;
    gfx_text(8, top, 1, C_DIM, "SEQ %02d  %d BARS  GRID %s  CURSOR %d.%d.%02d  %d EVENTS",
             A.e.cur_seq + 1, sq->bars, quant_names[sq->quant][0] == 'o' ? "1/16" : quant_names[sq->quant],
             bar + 1, beat + 1, tk, sq->nev);
    int ry = top + 16;
    for (uint32_t b = v0; b < v0 + vis; b += 4 * PPQ) {
        int x = gx + (int)((uint64_t)(b - v0) * gw / vis);
        gfx_text(x + 2, ry, 1, C_TEXT, "%u", b / (4 * PPQ) + 1);
    }
    int gtop = ry + 16;
    int rowh = (bottom - gtop) / NUM_PADS, ch = rowh - 2;

    /* background rows */
    for (int p = 0; p < NUM_PADS; p++) {
        int y = gtop + p * rowh;
        gfx_rect(gx, y, gw, ch, (p / PADS_PER_BANK) % 2 ? C_PANEL : 0x1f2228);
        if (p / PADS_PER_BANK == A.bank)
            gfx_rect(0, y, 4, ch, C_ACCENT);
        Color lc = p == A.cur_pad ? C_CURSOR : (A.e.pads[p].sample >= 0 ? C_TEXT : C_DIM);
        char nm[16];
        snprintf(nm, sizeof nm, "%c%d %s", 'A' + p / PADS_PER_BANK, p % PADS_PER_BANK + 1,
                 A.e.pads[p].label[0] ? A.e.pads[p].label : "--");
        nm[11] = 0;
        gfx_text(8, y + (ch - 12) / 2, 1, lc, "%s", nm);
        if (A.e.pad_flash[p] > 0.05f)
            gfx_rect(namew - 6, y + 2, 3, ch - 4, color_mix(C_BG, C_ACCENT, A.e.pad_flash[p]));
    }
    /* vertical lines: grid, beats, bars */
    int gh = NUM_PADS * rowh - 2;
    float px_per_grid = (float)gw * g / vis;
    for (uint32_t t = v0; t < v0 + vis; t += g) {
        int x = gx + (int)((uint64_t)(t - v0) * gw / vis);
        if (t % (4 * PPQ) == 0)
            gfx_vline(x, gtop, gtop + gh, C_DIM);
        else if (t % PPQ == 0)
            gfx_vline(x, gtop, gtop + gh, 0x4a505c);
        else if (px_per_grid >= 6)
            gfx_vline(x, gtop, gtop + gh, 0x2c3038);
    }
    /* events */
    int ew = (int)px_per_grid - 2;
    if (ew < 3)
        ew = 3;
    if (ew > 14)
        ew = 14;
    for (int i = 0; i < sq->nev; i++) {
        Event *ev = &sq->ev[i];
        if (ev->tick < v0 || ev->tick >= v0 + vis)
            continue;
        int x = gx + (int)((uint64_t)(ev->tick - v0) * gw / vis);
        int y = gtop + ev->pad * rowh;
        Color c = color_mix(C_STEPLO, C_STEP, ev->vel / 127.0f);
        gfx_rect(x + 1, y + 2, ew, ch - 4, c);
        if (ev->semi)
            gfx_rect(x + 1, y + 2 + (ch - 6) * (15 - (ev->semi > 15 ? 15 : ev->semi)) / 15, ew, 2, C_CURSOR);
    }
    /* cursor */
    int cx0 = gx + (int)((uint64_t)(A.grid_tick - v0) * gw / vis);
    int cx1 = gx + (int)((uint64_t)(A.grid_tick + g - v0) * gw / vis);
    gfx_frame(cx0, gtop + A.cur_pad * rowh - 1, cx1 - cx0 + 1, ch + 2, 2, C_CURSOR);
    /* playhead */
    if (A.e.playing) {
        double t = A.e.play_tick;
        if (t >= v0 && t < v0 + vis) {
            int x = gx + (int)((t - v0) * gw / vis);
            gfx_rect(x, gtop - 4, 2, gh + 4, C_PLAY);
        }
    }
}

void view_seq_key(SDLKey k, int rep, int shift)
{
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    uint32_t len = seq_len(sq);
    int g = grid_ticks();
    /* keep cursor on the grid */
    A.grid_tick = A.grid_tick / g * g;
    if (shift) { /* SHIFT: jump bars, zoom, mute row */
        if (k == BTN_LEFT)
            A.grid_tick = (A.grid_tick + len - 4 * PPQ) % len;
        if (k == BTN_RIGHT)
            A.grid_tick = (A.grid_tick + 4 * PPQ) % len;
        if (k == BTN_UP && !rep && A.grid_zoom > 0)
            A.grid_zoom--;
        if (k == BTN_DOWN && !rep && A.grid_zoom < NUM_ZOOM - 1)
            A.grid_zoom++;
        if (k == BTN_X && !rep) {
            Pad *p = &A.e.pads[A.cur_pad];
            undo_mark(&A.e, NULL);
            engine_lock();
            p->mute = !p->mute;
            engine_unlock();
            app_toast("Pad %d %s", A.cur_pad + 1, p->mute ? "muted" : "unmuted");
        }
        return;
    }
    if (k == BTN_LEFT)
        A.grid_tick = (A.grid_tick + len - g) % len / g * g;
    if (k == BTN_RIGHT)
        A.grid_tick = (A.grid_tick + g) % len;
    if (k == BTN_UP)
        A.cur_pad = (A.cur_pad + NUM_PADS - 1) % NUM_PADS;
    if (k == BTN_DOWN)
        A.cur_pad = (A.cur_pad + 1) % NUM_PADS;
    A.bank = A.cur_pad / PADS_PER_BANK;
    if (rep)
        return;
    uint32_t t0 = A.grid_tick, t1 = A.grid_tick + g;
    int n = seq_count_range(&A.e, A.e.cur_seq, A.cur_pad, t0, t1);
    if (k == BTN_A || (k == BTN_X && n))
        undo_mark(&A.e, NULL);
    if (k == BTN_A) {
        if (n)
            seq_remove_range(&A.e, A.e.cur_seq, A.cur_pad, t0, t1);
        else {
            if (seq_add(&A.e, A.e.cur_seq, t0, A.cur_pad, 110) < 0)
                app_toast("Sequence is full");
            if (!A.e.playing)
                engine_trigger(&A.e, A.cur_pad, 110);
        }
    }
    if (k == BTN_X && n) { /* cycle velocity: strong -> medium -> soft */
        int vel = 127;
        for (int i = 0; i < sq->nev; i++)
            if (sq->ev[i].pad == A.cur_pad && sq->ev[i].tick >= t0 && sq->ev[i].tick < t1) {
                vel = sq->ev[i].vel;
                break;
            }
        vel = vel >= 100 ? 75 : vel >= 60 ? 40
                                          : 120;
        seq_set_vel_range(&A.e, A.e.cur_seq, A.cur_pad, t0, t1, vel);
        if (!A.e.playing)
            engine_trigger(&A.e, A.cur_pad, vel);
    }
    if (k == BTN_B)
        engine_trigger(&A.e, A.cur_pad, 110);
    if (k == BTN_Y) { /* grid / quantize resolution (skips "off") */
        int q = sq->quant % (NUM_QUANT - 1) + 1;
        sq->quant = q;
        app_toast("Grid and quantize: %s", quant_names[q]);
    }
}

const char *view_seq_help(void)
{
    return "A add/del  X loud  B hear  Y grid  {SH}+Arrows bar/zoom";
}

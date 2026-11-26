/* SONG view: a chain of sequences, each played a number of times.
 * A switches song mode on/off (off = loop the current sequence). */
#include "app.h"
#include "help.h"
#include "undo.h"
#include <stdio.h>

void view_song_draw(int top, int bottom)
{
    Song *sg = &A.e.song;
    int x = 16, w = SCREEN_W - 32;
    gfx_text(x, top + 4, 1, C_DIM, "SONG MODE");
    gfx_text(x + 92, top + 4, 1, A.e.song_mode ? C_PLAY : C_TEXT, "%s",
             A.e.song_mode ? "ON - plays the list below" : "OFF - loops the current sequence");
    int y = top + 28, rowh = 20;
    int rows = (bottom - y - 4) / rowh;
    if (A.song_row > sg->n) A.song_row = sg->n;
    int first = A.song_row - rows / 2;
    if (first > sg->n + 1 - rows) first = sg->n + 1 - rows;
    if (first < 0) first = 0;
    gfx_text(x + 8, y, 1, C_DIM, "STEP");
    gfx_text(x + 70, y, 1, C_DIM, "SEQUENCE");
    gfx_text(x + 190, y, 1, C_DIM, "REPEAT");
    gfx_text(x + 270, y, 1, C_DIM, "BARS");
    y += rowh;
    for (int r = first; r <= sg->n && r < first + rows - 1; r++, y += rowh) {
        int sel = r == A.song_row;
        if (sel) gfx_rect(x, y - 3, w, rowh - 2, C_SEL);
        if (r == sg->n) {
            gfx_text(x + 8, y, 1, sel ? C_ACCENT : C_DIM, "%s", r ? "-- end (Y adds a step here)" : "-- empty: press Y to add a step");
            continue;
        }
        int playing = A.e.song_mode && A.e.playing && r == A.e.song_pos;
        Color c = playing ? C_PLAY : sel ? C_ACCENT : C_TEXT;
        Sequence *sq = &A.e.seqs[sg->seq[r] % MAX_SEQS];
        gfx_text(x + 8, y, 1, c, "%2d%s", r + 1, playing ? " >" : "");
        gfx_text(x + 70, y, 1, c, "SEQ %02d%s", sg->seq[r] + 1, sq->nev ? "" : " (empty)");
        gfx_text(x + 190, y, 1, c, "x%d", sg->reps[r]);
        gfx_text(x + 270, y, 1, c, "%d", sq->bars * sg->reps[r]);
        if (playing) gfx_text(x + 320, y, 1, C_PLAY, "pass %d/%d", A.e.song_rep + 1, sg->reps[r]);
    }
}

void view_song_key(SDLKey k, int rep, int shift)
{
    Song *sg = &A.e.song;
    if (shift && (k == BTN_UP || k == BTN_DOWN)) {
        if (A.song_row >= sg->n) return;
        undo_mark(&A.e, "songreps");
        int v = sg->reps[A.song_row] + (k == BTN_UP ? 1 : -1);
        sg->reps[A.song_row] = v < 1 ? 1 : v > 16 ? 16 : v;
        return;
    }
    if (k == BTN_UP && A.song_row > 0) A.song_row--;
    if (k == BTN_DOWN && A.song_row < sg->n) A.song_row++;
    if (rep && k != BTN_LEFT && k != BTN_RIGHT) return;
    if ((k == BTN_LEFT || k == BTN_RIGHT) && A.song_row < sg->n) {
        undo_mark(&A.e, "songseq");
        sg->seq[A.song_row] = (sg->seq[A.song_row] + (k == BTN_RIGHT ? 1 : MAX_SEQS - 1)) % MAX_SEQS;
    }
    if (k == BTN_A) {
        engine_lock();
        A.e.song_mode = !A.e.song_mode && sg->n > 0;
        engine_unlock();
        app_toast(A.e.song_mode ? "Song mode on: play starts at step 1" : sg->n ? "Song mode off" : "Add a step first (Y)");
    }
    if (k == BTN_Y && sg->n < MAX_SONG) {
        undo_mark(&A.e, NULL);
        engine_lock();
        int at = A.song_row;
        for (int i = sg->n; i > at; i--) { sg->seq[i] = sg->seq[i - 1]; sg->reps[i] = sg->reps[i - 1]; }
        sg->seq[at] = at > 0 ? sg->seq[at - 1] : A.e.cur_seq;
        sg->reps[at] = 1;
        sg->n++;
        engine_unlock();
    }
    if (k == BTN_B && A.song_row < sg->n) {
        undo_mark(&A.e, NULL);
        engine_lock();
        for (int i = A.song_row; i < sg->n - 1; i++) { sg->seq[i] = sg->seq[i + 1]; sg->reps[i] = sg->reps[i + 1]; }
        sg->n--;
        if (A.e.song_pos >= sg->n) A.e.song_pos = 0;
        if (!sg->n) A.e.song_mode = 0;
        engine_unlock();
    }
    if (k == BTN_X && A.song_row < sg->n) engine_select_seq(&A.e, sg->seq[A.song_row]);
}

const char *view_song_help(void)
{
    return "A song on/off  Y add  B del  {LR} seq  {SH}+{UD} repeat";
}

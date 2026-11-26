/* PLAY view: four pads on A/B/X/Y (laid out like the buttons), four banks
 * on Left/Right = 16 pads. Hold Down for note repeat, Up toggles KEYS mode
 * (the current pad played chromatically). */
#include "app.h"
#include "help.h"
#include "undo.h"
#include <stdio.h>
#include <string.h>

static const char *btn_name[PADS_PER_BANK] = {"X", "A", "B", "Y"};
static const char *note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static int pad_slot(SDLKey k)
{
    if (k == BTN_X)
        return 0;
    if (k == BTN_A)
        return 1;
    if (k == BTN_B)
        return 2;
    if (k == BTN_Y)
        return 3;
    return -1;
}

static int pad_in_seq(int p)
{
    const Sequence *s = &A.e.seqs[A.e.cur_seq];
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].pad == p)
            return 1;
    return 0;
}

/* In KEYS mode slot+bank give 0..15 semitones above the pad's own pitch. */
static int slot_semi(int slot) { return A.bank * PADS_PER_BANK + slot; }

static void draw_pad(int slot, int x, int y, int size)
{
    int p = A.keys_mode ? A.keys_pad : A.bank * PADS_PER_BANK + slot;
    Pad *pd = &A.e.pads[p];
    float fl = A.keys_mode ? A.e.pad_flash[p] * (A.cur_pad == p ? 1 : 0) : A.e.pad_flash[p];
    Color base = A.keys_mode ? 0x2f3a4f : (pd->sample >= 0 ? C_PAD : C_PANEL2);
    Color c = color_mix(base, C_ACCENT, fl);
    gfx_rect(x, y, size, size, c);
    if (!A.keys_mode && p == A.cur_pad)
        gfx_frame(x - 3, y - 3, size + 6, size + 6, 2, C_CURSOR);
    gfx_text(x + 8, y + 6, 2, fl > 0.4f ? C_BG : C_TEXT, "%s", btn_name[slot]);
    if (A.keys_mode) {
        int semi = slot_semi(slot);
        gfx_text(x + 8, y + size - 34, 2, C_TEXT, "%s", note_names[semi % 12]);
        gfx_text(x + size - 40, y + 8, 1, C_DIM, "+%d", semi);
        return;
    }
    gfx_text(x + size - 34, y + 8, 1, C_DIM, "P%d", p + 1);
    if (pad_in_seq(p))
        gfx_rect(x + size - 12, y + size - 12, 6, 6, C_STEP);
    if (pd->mute)
        gfx_text(x + size - 34, y + 22, 1, C_REC, "M");
    int maxc = (size - 14) / 8;
    char nm[32];
    snprintf(nm, sizeof nm, "%s", pd->label[0] ? pd->label : "empty");
    if ((int)strlen(nm) > maxc)
        nm[maxc] = 0;
    gfx_text(x + 8, y + size - 20, 1, pd->sample >= 0 ? C_TEXT : C_DIM, "%s", nm);
}

void view_play_draw(int top, int bottom)
{
    int lw = SCREEN_W * 54 / 100;
    int ah = bottom - top;
    int s = ah / 3 < lw / 3 ? ah / 3 : lw / 3;
    int size = s - 12;
    int cx = lw / 2 + 4, cy = top + ah / 2;
    draw_pad(0, cx - size / 2, cy - s - size / 2, size); /* X top    */
    draw_pad(1, cx + s - size / 2, cy - size / 2, size); /* A right  */
    draw_pad(2, cx - size / 2, cy + s - size / 2, size); /* B bottom */
    draw_pad(3, cx - s - size / 2, cy - size / 2, size); /* Y left   */

    int px = lw + 8, pw = SCREEN_W - px - 10, y = top;
    gfx_rect(px, top, pw, ah, C_PANEL);
    y += 8;
    gfx_text(px + 10, y, 1, C_DIM, "%s", A.keys_mode ? K("OCTAVE AREA (Left/Right)") : K("BANK (Left/Right)"));
    y += 16;
    int bw = (pw - 20 - 3 * 6) / 4;
    for (int b = 0; b < NUM_BANKS; b++) {
        int bx = px + 10 + b * (bw + 6);
        gfx_rect(bx, y, bw, 34, b == A.bank ? C_SEL : C_PANEL2);
        if (b == A.bank)
            gfx_frame(bx, y, bw, 34, 2, C_ACCENT);
        if (A.keys_mode)
            gfx_text(bx + 6, y + 3, 1, b == A.bank ? C_ACCENT : C_TEXT, "+%d", b * 4);
        else
            gfx_text(bx + 6, y + 3, 1, b == A.bank ? C_ACCENT : C_TEXT, "%c", 'A' + b);
        int q = (bw - 12) / 4;
        for (int i = 0; i < PADS_PER_BANK; i++) {
            int p = b * PADS_PER_BANK + i;
            gfx_rect(bx + 6 + i * q, y + 20, q - 3, 8,
                     color_mix(A.e.pads[p].sample >= 0 ? C_PAD : C_BG, C_ACCENT, A.keys_mode ? 0 : A.e.pad_flash[p]));
        }
    }
    y += 44;
    int pp = A.keys_mode ? A.keys_pad : A.cur_pad;
    Pad *pd = &A.e.pads[pp];
    gfx_text(px + 10, y, 1, C_DIM, A.keys_mode ? "KEYS" : "PAD %d", pp + 1);
    gfx_text(px + 70, y, 1, C_TEXT, "%.*s", (pw - 80) / 8, pd->label[0] ? pd->label : "(empty)");
    y += 20;
    Sequence *sq = &A.e.seqs[A.e.cur_seq];
    int fw = pw - 12;
    draw_field(px + 6, y, fw, 0, "SEQUENCE", "%02d   %d bars", A.e.cur_seq + 1, sq->bars);
    y += 18;
    draw_field(px + 6, y, fw, 0, "TEMPO", "%d BPM", A.e.bpm);
    y += 18;
    draw_field(px + 6, y, fw, 0, "QUANTIZE", "%s", quant_names[sq->quant]);
    y += 18;
    draw_field(px + 6, y, fw, 0, "MODE", "%s", A.keys_mode ? "KEYS (chromatic)" : "PADS");
    y += 18;
    draw_field(px + 6, y, fw, A.e.repeat_mask != 0, "REPEAT", "%s", key_held(BTN_DOWN) ? "on (holding)" : "hold Down");
    y += 24;

    if (A.e.rec) {
        gfx_rect(px + 10, y, pw - 20, 28, C_REC);
        gfx_text(px + 18, y + 8, 1, C_BG, "%s", A.e.playing ? (A.e.play_tick < 0 ? "COUNT-IN ..." : "RECORDING - play pads") : "REC ARMED");
    }
    else {
        gfx_frame(px + 10, y, pw - 20, 28, 1, C_DIM);
        gfx_text(px + 18, y + 8, 1, C_DIM, "%s", K("{SH}+{L2}: record"));
    }
    y += 36;
    if (y + 30 < bottom) {
        gfx_text(px + 10, y, 1, C_DIM, "%s", K("Up: PADS / KEYS mode"));
        gfx_text(px + 10, y + 16, 1, C_DIM, "%s", K("{SH} + pad: erase pad"));
    }
}

void view_play_key(SDLKey k, int rep, int shift)
{
    int slot = pad_slot(k);
    if (slot >= 0) {
        if (rep)
            return;
        int p = A.keys_mode ? A.keys_pad : A.bank * PADS_PER_BANK + slot;
        int semi = A.keys_mode ? slot_semi(slot) : 0;
        if (!A.keys_mode)
            A.cur_pad = p;
        if (shift && !A.keys_mode) {
            undo_mark(&A.e, NULL);
            seq_remove_range(&A.e, A.e.cur_seq, p, 0, 0xffffffffu);
            app_toast("Erased pad %d from sequence %02d", p + 1, A.e.cur_seq + 1);
            return;
        }
        engine_trigger_semi(&A.e, p, 110, semi);
        if (key_held(BTN_DOWN)) { /* note repeat */
            engine_lock();
            A.e.repeat_mask |= 1u << p;
            A.e.repeat_semi[p] = (int8_t)semi;
            engine_unlock();
        }
        if (A.e.rec && A.e.playing) {
            undo_mark(&A.e, "rec");
            engine_record_hit(&A.e, p, 110, semi);
        }
        return;
    }
    if (k == BTN_LEFT && !rep)
        A.bank = (A.bank + NUM_BANKS - 1) % NUM_BANKS;
    if (k == BTN_RIGHT && !rep)
        A.bank = (A.bank + 1) % NUM_BANKS;
    if ((k == BTN_LEFT || k == BTN_RIGHT) && !A.keys_mode)
        A.cur_pad = A.bank * PADS_PER_BANK + A.cur_pad % PADS_PER_BANK;
    if (k == BTN_UP && !rep) {
        A.keys_mode = !A.keys_mode;
        if (A.keys_mode) {
            A.keys_pad = A.cur_pad;
            A.bank = 0;
        }
        else
            A.bank = A.cur_pad / PADS_PER_BANK;
        app_toast(A.keys_mode ? "KEYS: pad %d across 16 semitones" : "PADS mode", A.keys_pad + 1);
    }
}

void view_play_keyup(SDLKey k)
{
    int slot = pad_slot(k);
    if (slot >= 0 || k == BTN_DOWN) {
        /* release note repeat for that pad (or all when Down is released) */
        engine_lock();
        if (k == BTN_DOWN)
            A.e.repeat_mask = 0;
        else {
            int p = A.keys_mode ? A.keys_pad : A.bank * PADS_PER_BANK + slot;
            if (!A.keys_mode)
                A.e.repeat_mask &= ~(1u << p);
            else
                A.e.repeat_mask = 0;
        }
        engine_unlock();
    }
}

const char *view_play_help(void)
{
    return A.keys_mode ? "A/B/X/Y notes  Left/Right octave  Up PADS  hold Down repeat"
                       : "A/B/X/Y pads  Left/Right bank  Up KEYS  hold Down repeat";
}

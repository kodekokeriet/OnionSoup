#include "help.h"
#include "app.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *tok, *miyoo, *pc; } KeyName;
static const KeyName names[] = {
    { "L1", "L1", "Q" },        { "R1", "R1", "W" },
    { "L2", "L2", "Space" },    { "R2", "R2", "Shift" },
    { "SH", "R2", "Shift" },
    { "SEL", "SELECT", "Tab" }, { "START", "START", "Enter" },
    { "MENU", "MENU", "Esc" },  { "DPAD", "D-pad", "Arrows" },
    { "UD", "Up/Down", "Up/Down" }, { "LR", "Left/Right", "Left/Right" },
    { "HELP", "MENU > Help", "F1" },
};

void keys_expand(const char *in, char *out, int n)
{
    int o = 0;
    while (*in && o < n - 1) {
        if (*in == '{') {
            const char *e = strchr(in, '}');
            if (e) {
                for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
                    size_t l = strlen(names[i].tok);
                    if ((size_t)(e - in - 1) == l && !strncmp(in + 1, names[i].tok, l)) {
#ifdef MIYOO
                        const char *r = names[i].miyoo;
#else
                        const char *r = names[i].pc;
#endif
                        while (*r && o < n - 1) out[o++] = *r++;
                        break;
                    }
                }
                in = e + 1;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

const char *K(const char *in)
{
    static char buf[4][160];
    static int n;
    char *b = buf[n++ & 3];
    keys_expand(in, b, 160);
    return b;
}

/* Per-view explanations. Each line: "keys|what it does". */
static const char *play_lines[] = {
    "A B X Y|play the 4 pads (placed like the buttons)",
    "{LR}|switch pad bank A-D (16 pads in total)",
    "Up|KEYS mode: current pad over 16 semitones",
    "hold Down + pad|note repeat at the quantize grid",
    "{SH} + pad|erase that pad from the sequence",
    NULL,
};
static const char *seq_lines[] = {
    "{DPAD}|move the cursor (pad rows / time)",
    "A|add or delete a hit at the cursor",
    "X / B|change how loud the hit is / hear the pad",
    "Y|grid size (also the record quantize)",
    "{SH} + {LR}|jump one bar back / forward",
    "{SH} + {UD}|zoom: 1, 2 or 4 bars",
    "{SH} + X|mute this pad row",
    NULL,
};
static const char *mix_lines[] = {
    "{SH} + {LR}|choose channel 1-16 or FX (master)",
    "{UD} / {LR}|choose a setting / change it",
    "X / Y|mute / solo the channel",
    "A|listen to the pad",
    "{SH} + B|reset the channel effects",
    "FX strip|delay, reverb, compressor, master level",
    NULL,
};
static const char *sample_lines[] = {
    "Y|load a WAV file onto this pad",
    "{UD} / {LR}|choose start, end, pitch, ... / change it",
    "A / B|hear the region / the whole file",
    "CHOP tap|A plays, X drops a mark, B removes it",
    "{SH} + A|chop: slices go onto the following pads",
    "{SH} + {LR}|previous / next pad",
    "{SH} + Y / B|copy / paste the pad settings",
    NULL,
};
static const char *song_lines[] = {
    "{UD}|choose a step in the song list",
    "Y / B|add a step / delete the step",
    "{LR}|which sequence the step plays",
    "{SH} + {UD}|how many times it repeats",
    "A|song mode on / off",
    "X|make that sequence the current one",
    NULL,
};

static void line(int x, int y, int keyw, const char *src)
{
    char buf[160], keys[64];
    keys_expand(src, buf, sizeof buf);
    char *bar = strchr(buf, '|');
    if (!bar) { gfx_text(x, y, 1, C_TEXT, "%s", buf); return; }
    *bar = 0;
    snprintf(keys, sizeof keys, "%s", buf);
    gfx_text(x, y, 1, C_ACCENT, "%s", keys);
    gfx_text(x + keyw, y, 1, C_TEXT, "%s", bar + 1);
}

void help_draw(int view)
{
    int m = SCREEN_W / 24, x = m, y = m, w = SCREEN_W - 2 * m, h = SCREEN_H - 2 * m;
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_frame(x, y, w, h, 2, C_ACCENT);
    static const char *vname[NUM_VIEWS] = { "PLAY", "SEQ", "MIX", "SAMPLE", "SONG" };
    int tx = x + 14, ty = y + 10;
    gfx_text(tx, ty, 2, C_ACCENT, "HELP");
    gfx_text(tx + 80, ty + 8, 1, C_DIM, "any key closes");
    ty += 34;

#ifndef MIYOO
    gfx_text(tx, ty, 1, C_DIM, "KEYBOARD = MIYOO BUTTON");
    ty += 18;
    static const char *keymap[][2] = {
        { "Arrows", "D-pad" }, { "A B X Y", "A B X Y" }, { "Q / W", "L1 / R1" },
        { "Space / 1", "L2" }, { "Shift / 2", "R2 (SHIFT)" }, { "Tab", "SELECT" },
        { "Enter", "START" }, { "Esc", "MENU" }, { "F1", "this help" },
    };
    int colw = (w - 28) / 2;
    for (int i = 0; i < 9; i++) {
        int cx = tx + (i % 2) * colw, cy = ty + (i / 2) * 16;
        gfx_text(cx, cy, 1, C_ACCENT, "%s", keymap[i][0]);
        gfx_text(cx + 88, cy, 1, C_TEXT, "%s", keymap[i][1]);
    }
    ty += 5 * 16 + 8;
#endif
    char b[160];
    gfx_text(tx, ty, 1, C_DIM, "EVERYWHERE");
    ty += 18;
    static const char *every[] = {
        "{L1} / {R1}|previous / next view",
        "{L2}|play / stop",
        "{SH} + {L2}|record (count-in if set in settings)",
        "{SH} + {L1} / {R1}|undo / redo",
        "{SEL}|settings: tempo, tap, swing, quantize, ...",
        "{START} / {MENU}|project: save, open, resample, export",
    };
    for (int i = 0; i < 6; i++, ty += 16) { keys_expand(every[i], b, sizeof b); line(tx, ty, 160, b); }
    ty += 8;

    const char **lines = view == VIEW_PLAY ? play_lines : view == VIEW_SEQ ? seq_lines
                       : view == VIEW_MIX ? mix_lines : view == VIEW_SAMPLE ? sample_lines : song_lines;
    gfx_text(tx, ty, 1, C_DIM, "THIS VIEW: %s", vname[view]);
    ty += 18;
    for (int i = 0; lines[i] && ty < y + h - 16; i++, ty += 16) line(tx, ty, 160, lines[i]);
}

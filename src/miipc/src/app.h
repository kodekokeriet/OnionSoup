/* Application state shared between the views. */
#ifndef APP_H
#define APP_H
#include "browser.h"
#include "engine.h"
#include "gfx.h"
#include "input.h"
#include "theme.h"

enum { VIEW_PLAY,
       VIEW_SEQ,
       VIEW_MIX,
       VIEW_SAMPLE,
       VIEW_SONG,
       NUM_VIEWS };
enum { BR_NONE,
       BR_SAMPLE,
       BR_PROJECT };

#define HEADER_H 46
#define FOOTER_H 22
#define MIX_FX NUM_PADS /* mixer "channel" index for the FX / master strip */

typedef struct {
    Engine e;
    int view;
    int cur_pad;
    int bank;

    Browser br;
    int br_purpose;
    Sample preview; /* file browser audition */

    char project_path[512]; /* empty = not saved yet */
    char sample_dir[512];   /* last folder used in the browser */
    char toast[128];
    Uint32 toast_until;

    int menu_open, menu_sel, menu_confirm;
    int help_open;
    int settings_open;
    int snap;      /* snap chop points / start / end to zero crossings */
    int keys_mode; /* PLAY: A/B/X/Y + banks play the current pad chromatically */
    int keys_pad;
    int song_row;
    uint32_t tap_marks[16]; /* SAMPLE: tap-chop markers */
    int ntap;
    Pad clip; /* copied pad */
    int clip_valid;
    char status[32]; /* battery text, Miyoo / laptop */

    int play_field;
    uint32_t grid_tick;
    int grid_zoom;
    int mix_ch, mix_field;
    int smp_field, chop_mode;

    float meter[NUM_PADS]; /* smoothed for display */
    float mmeter[2];
} App;

extern App A;

int key_held(SDLKey k);
void app_toast(const char *fmt, ...);
Color color_mix(Color a, Color b, float t);
void draw_field(int x, int y, int w, int selected, const char *label, const char *fmt, ...);

void view_play_draw(int top, int bottom);
void view_play_key(SDLKey k, int rep, int shift);
const char *view_play_help(void);

void view_seq_draw(int top, int bottom);
void view_seq_key(SDLKey k, int rep, int shift);
const char *view_seq_help(void);

void view_mix_draw(int top, int bottom);
void view_mix_key(SDLKey k, int rep, int shift);
const char *view_mix_help(void);

void view_sample_draw(int top, int bottom);
void view_sample_key(SDLKey k, int rep, int shift);
const char *view_sample_help(void);

void view_play_keyup(SDLKey k);

void view_song_draw(int top, int bottom);
void view_song_key(SDLKey k, int rep, int shift);
const char *view_song_help(void);

#define BTN_SHIFT BTN_R2 /* R2 held = SHIFT */

void open_sample_browser(void);

#endif

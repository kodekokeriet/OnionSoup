/* Audio engine: polyphonic sample playback, linear sequencer and mixer,
 * all running in the SDL audio callback for sample-accurate timing. */
#ifndef ENGINE_H
#define ENGINE_H
#include "fx.h"
#include "sample.h"

#define NUM_PADS      16
#define PADS_PER_BANK 4
#define NUM_BANKS     (NUM_PADS / PADS_PER_BANK)
#define MAX_VOICES    16
#define MAX_SAMPLES   48

#define PPQ           96      /* ticks per quarter note */
#define MAX_SEQS      16
#define MAX_BARS      32
#define MAX_EVENTS    2048
#define NUM_QUANT     6
extern const int   quant_ticks[NUM_QUANT];  /* 0 = off */
extern const char *quant_names[NUM_QUANT];
enum { CLICK_OFF, CLICK_ON, CLICK_REC, NUM_CLICK };
extern const char *click_names[NUM_CLICK];

typedef struct {
    /* sample / voice */
    int      sample;    /* index into the sample pool, -1 = empty */
    uint32_t start;     /* region in sample frames (chops) */
    uint32_t end;
    int      pitch;     /* semitones, -12..12 */
    int      choke;     /* 0 = none, else group 1..4 */
    float    attack;    /* ms */
    float    decay;     /* ms, 0 = play to end */
    char     label[24];
    /* mixer channel */
    float    level;     /* 0..1 */
    float    pan;       /* -1..1 */
    int      mute, solo;
    int      filter;    /* FILT_* */
    float    cutoff;    /* 0..1 (20 Hz .. 20 kHz) */
    float    reso;      /* 0..1 */
    float    drive;     /* 0..1 */
    int      crush;     /* index into crush_bits */
    float    send_dly;  /* 0..1 */
    float    send_rev;  /* 0..1 */
} Pad;

typedef struct {
    uint32_t tick;
    uint8_t  pad, vel, flags;
    int8_t   semi;      /* pitch offset in semitones (KEYS mode), 0 = as set on the pad */
} Event;
#define EV_SKIP 1 /* recorded ahead of the playhead: don't play again this pass */

typedef struct {
    int   bars;
    int   quant;            /* index into quant_ticks */
    int   nev;
    Event ev[MAX_EVENTS];   /* sorted by tick */
} Sequence;

#define MAX_SONG 32
typedef struct {
    int     n;
    uint8_t seq[MAX_SONG];
    uint8_t reps[MAX_SONG];
} Song;

typedef struct {
    Sample   pool[MAX_SAMPLES];
    Pad      pads[NUM_PADS];
    Sequence seqs[MAX_SEQS];
    int      cur_seq, next_seq;
    Song     song;
    int      song_mode;     /* 1 = play the song chain instead of looping one sequence */
    int      song_pos;      /* read by UI */
    int      song_rep;
    FxParams fx;
    int      bpm;
    float    swing;         /* 0..0.5, applied to 16ths */
    int      click;         /* CLICK_* */
    int      rec;           /* record armed (UI) */
    int      count_in;      /* 1 = one bar of click before recording starts */
    int      playing;
    uint32_t repeat_mask;   /* pads held for note repeat (bit per pad) */
    int8_t   repeat_semi[NUM_PADS];
    float    cpu_load;      /* audio callback time / buffer time, 0..1 */
    double   play_tick;     /* musical tick position, read by UI */
    int      out_rate;
    float    pad_flash[NUM_PADS];
    float    pad_meter[NUM_PADS];   /* peak since last UI read */
    float    master_meter[2];
} Engine;

int  engine_init(Engine *e);
void engine_quit(Engine *e);
void engine_reset(Engine *e);            /* new empty project */
void pad_defaults(Pad *p);

int  engine_load_sample(Engine *e, const char *path);  /* -> pool index or -1 */
void engine_assign(Engine *e, int pad, int sample);    /* whole file on pad */
void engine_gc(Engine *e);               /* free samples no pad uses */
Sample *engine_pad_sample(Engine *e, int pad);

void engine_trigger(Engine *e, int pad, int velocity);
void engine_trigger_semi(Engine *e, int pad, int velocity, int semi);
void engine_preview(Engine *e, const Sample *s, uint32_t start, uint32_t end);
void engine_stop_preview(Engine *e);
void engine_set_playing(Engine *e, int on);
void engine_start_recording(Engine *e);   /* play + rec, with optional count-in */
double engine_preview_pos(void);           /* preview playhead in frames, -1 = idle */
/* Offline render of one pass of a sequence (+ tail) to 16-bit stereo. Stops
 * playback while rendering. Returns frames, *out is malloc'ed. */
int  engine_render(Engine *e, int seq, float tail_sec, int16_t **out);

/* Sequencer editing (all take the audio lock internally). */
static inline uint32_t seq_len(const Sequence *s) { return (uint32_t)s->bars * 4 * PPQ; }
int  engine_record_hit(Engine *e, int pad, int velocity, int semi);  /* live record, quantized */
int  seq_add(Engine *e, int seq, uint32_t tick, int pad, int vel);
int  seq_add_semi(Engine *e, int seq, uint32_t tick, int pad, int vel, int semi);
void seq_remove_range(Engine *e, int seq, int pad, uint32_t t0, uint32_t t1); /* pad -1 = all */
int  seq_count_range(const Engine *e, int seq, int pad, uint32_t t0, uint32_t t1);
void seq_set_vel_range(Engine *e, int seq, int pad, uint32_t t0, uint32_t t1, int vel);
void seq_set_bars(Engine *e, int seq, int bars);
void seq_copy(Engine *e, int src, int dst);
void engine_select_seq(Engine *e, int seq); /* queued to loop end while playing */
void engine_resync(Engine *e);              /* after replacing sequences (undo) */

void engine_lock(void);
void engine_unlock(void);

#endif

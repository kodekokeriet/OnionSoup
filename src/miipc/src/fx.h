/* Effects: per-voice insert chain (filter, drive, bitcrush) and master
 * bus (tempo-synced ping-pong delay, reverb, compressor, soft limiter). */
#ifndef FX_H
#define FX_H

enum { FILT_OFF, FILT_LP, FILT_HP, FILT_BP, NUM_FILT };
extern const char *filter_names[NUM_FILT];

#define NUM_CRUSH 5
extern const int   crush_bits[NUM_CRUSH];   /* 0 = off */

#define NUM_DLY_SYNC 6
extern const char *dly_sync_names[NUM_DLY_SYNC];
extern const float dly_sync_beats[NUM_DLY_SYNC];

typedef struct {
    int   dly_sync;     /* index into dly_sync_* */
    float dly_fb;       /* 0..0.95 */
    float dly_level;    /* return level 0..1 */
    float rev_size;     /* 0..1 */
    float rev_damp;     /* 0..1 */
    float rev_level;    /* return level 0..1 */
    float comp_thresh;  /* dB, -40..0 (0 = off) */
    float comp_ratio;   /* 1..20 */
    float master;       /* 0..1.5 */
} FxParams;

void fx_defaults(FxParams *p);

/* Zero-delay-feedback state-variable filter (Cytomic/TPT form). */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float k, a1, a2, a3; int type; } SvfCoef;

void svf_coef(SvfCoef *c, int type, float cutoff01, float reso01, int rate);

static inline float svf_run(Svf *s, const SvfCoef *c, float x)
{
    float v3 = x - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    switch (c->type) {
    case FILT_LP: return v2;
    case FILT_HP: return x - c->k * v1 - v2;
    case FILT_BP: return v1;
    default:      return x;
    }
}

/* Cheap tanh-like soft clipper. */
static inline float soft_clip(float x)
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

int  fx_init(int rate);
void fx_quit(void);
void fx_clear(void);
/* Processes one stereo frame of the master bus in place. dly_in/rev_in are
 * the summed send signals. delay_frames is the current delay time. */
void fx_master(float *l, float *r, float dly_in, float rev_in, const FxParams *p, float delay_frames);

#endif

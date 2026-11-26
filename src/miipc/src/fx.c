#include "fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *filter_names[NUM_FILT] = { "off", "low-pass", "high-pass", "band-pass" };
const int   crush_bits[NUM_CRUSH] = { 0, 12, 8, 6, 4 };
const char *dly_sync_names[NUM_DLY_SYNC] = { "1/16", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2" };
const float dly_sync_beats[NUM_DLY_SYNC] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };

void fx_defaults(FxParams *p)
{
    p->dly_sync = 2;
    p->dly_fb = 0.35f;
    p->dly_level = 0.6f;
    p->rev_size = 0.6f;
    p->rev_damp = 0.4f;
    p->rev_level = 0.5f;
    p->comp_thresh = 0.0f;
    p->comp_ratio = 4.0f;
    p->master = 0.9f;
}

void svf_coef(SvfCoef *c, int type, float cutoff01, float reso01, int rate)
{
    float fc = 20.0f * powf(1000.0f, cutoff01);       /* 20 Hz .. 20 kHz */
    if (fc > rate * 0.45f) fc = rate * 0.45f;
    float g = tanf(3.14159265f * fc / rate);
    c->k = 2.0f - 1.96f * reso01;                      /* Q 0.5 .. ~25 */
    c->a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a2 = g * c->a1;
    c->a3 = g * c->a2;
    c->type = type;
}

/* ---- delay ---- */
static float *dl_l, *dl_r;
static int    dl_size, dl_w;
static float  dl_lp_l, dl_lp_r;

/* ---- reverb (Schroeder/Moorer style, 6 combs + 4 allpasses per side) ---- */
#define NCOMB 6
#define NAP   4
static const int comb_len[NCOMB] = { 1116, 1188, 1277, 1356, 1422, 1491 };
static const int ap_len[NAP] = { 556, 441, 341, 225 };
#define SPREAD 23
typedef struct { float *buf; int len, i; float filt; } Comb;
typedef struct { float *buf; int len, i; } Allpass;
static Comb    combs[2][NCOMB];
static Allpass aps[2][NAP];

/* ---- compressor ---- */
static float comp_env, comp_gain = 1.0f;
static int   comp_ctr;
static float comp_att, comp_rel;
static int   srate;

int fx_init(int rate)
{
    srate = rate;
    dl_size = rate * 3; /* up to 3 s (1/2 note at 40 BPM) */
    dl_l = calloc(dl_size, sizeof(float));
    dl_r = calloc(dl_size, sizeof(float));
    float scale = rate / 44100.0f;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NCOMB; i++) {
            Comb *c = &combs[ch][i];
            c->len = (int)((comb_len[i] + ch * SPREAD) * scale);
            c->buf = calloc(c->len, sizeof(float));
        }
        for (int i = 0; i < NAP; i++) {
            Allpass *a = &aps[ch][i];
            a->len = (int)((ap_len[i] + ch * SPREAD) * scale);
            a->buf = calloc(a->len, sizeof(float));
        }
    }
    comp_att = 1.0f - expf(-1.0f / (0.003f * rate));
    comp_rel = 1.0f - expf(-1.0f / (0.150f * rate));
    return dl_l && dl_r ? 0 : -1;
}

void fx_clear(void)
{
    if (dl_l) memset(dl_l, 0, sizeof(float) * dl_size);
    if (dl_r) memset(dl_r, 0, sizeof(float) * dl_size);
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NCOMB; i++) if (combs[ch][i].buf) { memset(combs[ch][i].buf, 0, sizeof(float) * combs[ch][i].len); combs[ch][i].filt = 0; }
        for (int i = 0; i < NAP; i++) if (aps[ch][i].buf) memset(aps[ch][i].buf, 0, sizeof(float) * aps[ch][i].len);
    }
}

void fx_quit(void)
{
    free(dl_l); free(dl_r); dl_l = dl_r = NULL;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NCOMB; i++) { free(combs[ch][i].buf); combs[ch][i].buf = NULL; }
        for (int i = 0; i < NAP; i++) { free(aps[ch][i].buf); aps[ch][i].buf = NULL; }
    }
}

static inline float reverb_side(int ch, float in, float fb, float damp)
{
    float out = 0;
    for (int i = 0; i < NCOMB; i++) {
        Comb *c = &combs[ch][i];
        float y = c->buf[c->i];
        c->filt = y * (1.0f - damp) + c->filt * damp;
        c->buf[c->i] = in + c->filt * fb;
        if (++c->i >= c->len) c->i = 0;
        out += y;
    }
    for (int i = 0; i < NAP; i++) {
        Allpass *a = &aps[ch][i];
        float b = a->buf[a->i];
        float y = b - out;
        a->buf[a->i] = out + b * 0.5f;
        if (++a->i >= a->len) a->i = 0;
        out = y;
    }
    return out;
}

void fx_master(float *l, float *r, float dly_in, float rev_in, const FxParams *p, float delay_frames)
{
    if (!dl_l) return;
    /* Ping-pong delay with a gentle low-pass in the feedback path. */
    int d = (int)delay_frames;
    if (d < 1) d = 1;
    if (d >= dl_size) d = dl_size - 1;
    int rd = dl_w - d;
    if (rd < 0) rd += dl_size;
    float ol = dl_l[rd], or_ = dl_r[rd];
    dl_lp_l += 0.6f * (ol - dl_lp_l);
    dl_lp_r += 0.6f * (or_ - dl_lp_r);
    dl_l[dl_w] = dly_in + dl_lp_r * p->dly_fb;
    dl_r[dl_w] = dl_lp_l * p->dly_fb;
    if (++dl_w >= dl_size) dl_w = 0;

    /* Reverb */
    float fb = 0.70f + p->rev_size * 0.28f;
    float damp = p->rev_damp * 0.6f;
    float rin = rev_in * 0.08f;
    float rl = reverb_side(0, rin, fb, damp);
    float rr = reverb_side(1, rin, fb, damp);

    float L = *l + ol * p->dly_level + rl * p->rev_level;
    float R = *r + or_ * p->dly_level + rr * p->rev_level;

    /* Compressor: peak detector, gain recomputed every 8 frames. */
    if (p->comp_thresh < -0.5f) {
        float pk = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R);
        comp_env += (pk > comp_env ? comp_att : comp_rel) * (pk - comp_env);
        if ((++comp_ctr & 7) == 0) {
            float db = 20.0f * log10f(comp_env + 1e-6f);
            float over = db - p->comp_thresh;
            float gr = over > 0 ? over * (1.0f - 1.0f / p->comp_ratio) : 0.0f;
            float makeup = -p->comp_thresh * (1.0f - 1.0f / p->comp_ratio) * 0.5f;
            comp_gain = powf(10.0f, (makeup - gr) / 20.0f);
        }
        L *= comp_gain; R *= comp_gain;
    }
    *l = soft_clip(L * p->master);
    *r = soft_clip(R * p->master);
}

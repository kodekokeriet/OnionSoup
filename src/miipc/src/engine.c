#include "engine.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const int quant_ticks[NUM_QUANT] = {0, PPQ / 2, PPQ / 3, PPQ / 4, PPQ / 6, PPQ / 8};
const char *quant_names[NUM_QUANT] = {"off", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const char *click_names[NUM_CLICK] = {"off", "on", "rec only"};

typedef struct {
    int active;
    int pad; /* -1 = preview */
    int semi;
    const Sample *s;
    double pos, end, step;
    float vel;
    float env, att_inc, dec_mul;
    int fade; /* >0: fading out (choke/retrigger), frames left */
    Svf svf;
    float crush_hold;
    int crush_ctr;
} Voice;

#define FADE_FRAMES 96
static Voice voices[MAX_VOICES + 1]; /* last slot = preview */
static Engine *eng;
static double real_tick; /* un-swung tick position */
static int play_idx;     /* next event to fire */
static int last_beat = -1;
static float click_phase, click_env, click_freq;
static int rendering;   /* offline render: no click */
static double rep_tick; /* note repeat clock when stopped */

static int insert_locked(Engine *e, int si, uint32_t tick, int pad, int vel, int semi, int flags);

/* ---------------- voices ---------------- */

static Voice *alloc_voice(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (!voices[i].active)
            return &voices[i];
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].fade)
            return &voices[i];
    int best = 0;
    double bp = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        double p = voices[i].pos / (voices[i].end + 1);
        if (p > bp) {
            bp = p;
            best = i;
        }
    }
    return &voices[best];
}

static void start_voice(int pad, int vel, int semi)
{
    Pad *p = &eng->pads[pad];
    if (p->sample < 0 || !eng->pool[p->sample].data || p->end <= p->start + 1)
        return;
    const Sample *s = &eng->pool[p->sample];

    /* Same pad retriggers (mono per pad); a choke group cuts its members.
     * Cut voices fade out quickly instead of clicking. */
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (!v->active || v->fade)
            continue;
        /* KEYS mode: different notes of one pad may ring together */
        if ((v->pad == pad && v->semi == semi) || (p->choke && eng->pads[v->pad].choke == p->choke && v->pad != pad))
            v->fade = FADE_FRAMES;
    }
    Voice *v = alloc_voice();
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->pad = pad;
    v->semi = semi;
    v->s = s;
    v->pos = p->start;
    v->end = p->end < s->frames ? p->end : s->frames;
    v->step = (double)s->rate / eng->out_rate * pow(2.0, (p->pitch + semi) / 12.0);
    v->vel = vel / 127.0f;
    if (p->attack > 0.5f) {
        v->env = 0;
        v->att_inc = 1000.0f / (p->attack * eng->out_rate);
    }
    else {
        v->env = 1;
        v->att_inc = 0;
    }
    v->dec_mul = p->decay > 0.5f ? expf(-6.9f / (p->decay * 0.001f * eng->out_rate)) : 1.0f; /* -60 dB over decay */
    eng->pad_flash[pad] = 1.0f;
}

/* ---------------- sequencer ---------------- */

/* Swing: within each 8th note the first 16th is stretched and the second
 * squeezed. Maps real (straight) ticks to musical ticks. */
static double swing_warp(double r, float sw)
{
    if (sw <= 0.0f)
        return r;
    const double pair = PPQ / 2.0, half = PPQ / 4.0;
    double base = floor(r / pair) * pair, x = r - base;
    double split = half * (1.0 + sw);
    double m = x < split ? x / (1.0 + sw) : half + (x - split) / (1.0 - sw);
    return base + m;
}

static void fire_click(int beat)
{
    int bar_start = (beat % 4) == 0;
    click_freq = bar_start ? 1760.0f : 1175.0f;
    click_env = bar_start ? 0.5f : 0.3f;
    click_phase = 0;
}

/* Note repeat: held pads retrigger on every quantize step (1/16 if off). */
static void note_repeat(double from, double to, int record)
{
    if (!eng->repeat_mask)
        return;
    Sequence *sq = &eng->seqs[eng->cur_seq];
    int q = quant_ticks[sq->quant] ? quant_ticks[sq->quant] : PPQ / 4;
    if (floor(to / q) == floor(from / q))
        return;
    uint32_t tick = (uint32_t)(floor(to / q) * q);
    for (int p = 0; p < NUM_PADS; p++) {
        if (!(eng->repeat_mask & (1u << p)))
            continue;
        start_voice(p, 105, eng->repeat_semi[p]);
        if (record && tick < seq_len(sq)) {
            int dup = 0;
            for (int i = 0; i < sq->nev; i++)
                if (sq->ev[i].tick == tick && sq->ev[i].pad == p)
                    dup = 1;
            if (!dup)
                insert_locked(eng, eng->cur_seq, tick, p, 105, eng->repeat_semi[p], 0);
        }
    }
}

static void song_advance(void)
{
    Song *sg = &eng->song;
    if (!eng->song_mode || sg->n == 0)
        return;
    if (++eng->song_rep >= (sg->reps[eng->song_pos] ? sg->reps[eng->song_pos] : 1)) {
        eng->song_rep = 0;
        eng->song_pos = (eng->song_pos + 1) % sg->n;
    }
    eng->cur_seq = eng->next_seq = sg->seq[eng->song_pos] % MAX_SEQS;
}

static void seq_tick_frame(void)
{
    Sequence *sq = &eng->seqs[eng->cur_seq];
    double len = seq_len(sq);
    double prev_m = eng->play_tick;
    real_tick += eng->bpm * (double)PPQ / (60.0 * eng->out_rate);
    if (real_tick < 0) {
        /* count-in bar: click only */
        eng->play_tick = real_tick;
        int beat = (int)floor(real_tick / PPQ);
        if (beat != last_beat) {
            last_beat = beat;
            fire_click(beat + 4);
        }
        return;
    }
    if (real_tick >= len) {
        real_tick -= len;
        if (eng->song_mode)
            song_advance();
        else if (eng->next_seq != eng->cur_seq)
            eng->cur_seq = eng->next_seq;
        sq = &eng->seqs[eng->cur_seq];
        play_idx = 0;
        last_beat = -1;
        prev_m = -1;
        for (int i = 0; i < sq->nev; i++)
            sq->ev[i].flags &= ~EV_SKIP;
    }
    double m = swing_warp(real_tick, eng->swing);
    eng->play_tick = m;
    while (play_idx < sq->nev && sq->ev[play_idx].tick <= m) {
        Event *ev = &sq->ev[play_idx++];
        if (ev->flags & EV_SKIP) {
            ev->flags &= ~EV_SKIP;
            continue;
        }
        start_voice(ev->pad, ev->vel, ev->semi);
    }
    note_repeat(prev_m, m, eng->rec);
    int beat = (int)(m / PPQ);
    if (beat != last_beat) {
        last_beat = beat;
        if (!rendering && (eng->click == CLICK_ON || (eng->click == CLICK_REC && eng->rec)))
            fire_click(beat);
    }
}

/* ---------------- audio callback ---------------- */

typedef struct {
    SvfCoef svf;
    float gl, gr, sd, sr, drive_in, drive_out, crush_q;
    int crush_step;
} Chan;

static void render(int16_t *out, int frames)
{
    /* Per-channel coefficients, once per buffer. */
    Chan ch[NUM_PADS];
    int any_solo = 0;
    for (int p = 0; p < NUM_PADS; p++)
        any_solo |= eng->pads[p].solo;
    for (int p = 0; p < NUM_PADS; p++) {
        Pad *pd = &eng->pads[p];
        Chan *c = &ch[p];
        svf_coef(&c->svf, pd->filter, pd->cutoff, pd->reso, eng->out_rate);
        float g = pd->level;
        if (pd->mute || (any_solo && !pd->solo))
            g = 0;
        c->gl = g * (pd->pan <= 0 ? 1.0f : 1.0f - pd->pan);
        c->gr = g * (pd->pan >= 0 ? 1.0f : 1.0f + pd->pan);
        c->sd = g * pd->send_dly;
        c->sr = g * pd->send_rev;
        c->drive_in = 1.0f + pd->drive * 9.0f;
        c->drive_out = pd->drive > 0 ? 1.0f / soft_clip(c->drive_in) : 1.0f;
        int bits = crush_bits[pd->crush];
        c->crush_q = bits ? (float)(1 << (bits - 1)) : 0;
        c->crush_step = bits ? (16 - bits) / 3 + 1 : 1; /* lower bits also lower the rate */
    }
    float delay_frames = dly_sync_beats[eng->fx.dly_sync] * 60.0f / eng->bpm * eng->out_rate;
    float mpk_l = 0, mpk_r = 0;

    for (int f = 0; f < frames; f++) {
        if (eng->playing)
            seq_tick_frame();
        else if (eng->repeat_mask) { /* note repeat while stopped */
            double prev = rep_tick;
            rep_tick += eng->bpm * (double)PPQ / (60.0 * eng->out_rate);
            note_repeat(prev, rep_tick, 0);
        }
        float l = 0, r = 0, sd = 0, sr = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &voices[i];
            if (!v->active)
                continue;
            const Sample *s = v->s;
            if (v->pos + 1 >= v->end) {
                v->active = 0;
                continue;
            }
            uint32_t ip = (uint32_t)v->pos;
            float fr = (float)(v->pos - ip), a;
            if (s->channels == 2)
                a = 0.5f * (s->data[ip * 2] + fr * (s->data[ip * 2 + 2] - s->data[ip * 2]) +
                            s->data[ip * 2 + 1] + fr * (s->data[ip * 2 + 3] - s->data[ip * 2 + 1]));
            else
                a = s->data[ip] + fr * (s->data[ip + 1] - s->data[ip]);
            float x = a * (1.0f / 32768.0f);

            /* amp envelope */
            if (v->att_inc > 0) {
                v->env += v->att_inc;
                if (v->env >= 1) {
                    v->env = 1;
                    v->att_inc = 0;
                }
            }
            else
                v->env *= v->dec_mul;
            float g = v->env * v->vel;
            double left = (v->end - v->pos) / v->step;
            if (left < 128)
                g *= (float)(left / 128.0);
            if (v->fade) {
                g *= v->fade / (float)FADE_FRAMES;
                if (--v->fade == 0)
                    v->active = 0;
            }
            if (v->env < 0.0005f && v->att_inc == 0)
                v->active = 0;

            Chan *c = &ch[v->pad];
            x *= g;
            if (c->svf.type != FILT_OFF)
                x = svf_run(&v->svf, &c->svf, x);
            if (c->drive_out != 1.0f)
                x = soft_clip(x * c->drive_in) * c->drive_out;
            if (c->crush_q > 0) {
                if (v->crush_ctr-- <= 0) {
                    v->crush_ctr = c->crush_step - 1;
                    v->crush_hold = floorf(x * c->crush_q + 0.5f) / c->crush_q;
                }
                x = v->crush_hold;
            }
            float ax = fabsf(x) * (c->gl > c->gr ? c->gl : c->gr);
            if (ax > eng->pad_meter[v->pad])
                eng->pad_meter[v->pad] = ax;
            l += x * c->gl;
            r += x * c->gr;
            sd += x * c->sd;
            sr += x * c->sr;
            v->pos += v->step;
        }
        fx_master(&l, &r, sd, sr, &eng->fx, delay_frames);

        /* preview + metronome bypass the mixer */
        Voice *pv = &voices[MAX_VOICES];
        if (pv->active) {
            if (pv->pos + 1 >= pv->end)
                pv->active = 0;
            else {
                const Sample *s = pv->s;
                uint32_t ip = (uint32_t)pv->pos;
                float a = s->channels == 2 ? 0.5f * (s->data[ip * 2] + s->data[ip * 2 + 1]) : s->data[ip];
                l += a * (0.7f / 32768.0f);
                r += a * (0.7f / 32768.0f);
                pv->pos += pv->step;
            }
        }
        if (click_env > 0.001f) {
            float c = sinf(click_phase) * click_env;
            click_phase += 6.2831853f * click_freq / eng->out_rate;
            click_env *= 0.9985f;
            l += c;
            r += c;
        }
        if (fabsf(l) > mpk_l)
            mpk_l = fabsf(l);
        if (fabsf(r) > mpk_r)
            mpk_r = fabsf(r);
        l = l > 1 ? 1 : l < -1 ? -1
                               : l;
        r = r > 1 ? 1 : r < -1 ? -1
                               : r;
        out[f * 2] = (int16_t)(l * 32767.0f);
        out[f * 2 + 1] = (int16_t)(r * 32767.0f);
    }
    if (mpk_l > eng->master_meter[0])
        eng->master_meter[0] = mpk_l;
    if (mpk_r > eng->master_meter[1])
        eng->master_meter[1] = mpk_r;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    double t0 = now_sec();
    int frames = len / 4;
    render((int16_t *)stream, frames);
    double load = (now_sec() - t0) / ((double)frames / eng->out_rate);
    eng->cpu_load += (float)(load - eng->cpu_load) * 0.1f;
}

/* ---------------- setup ---------------- */

void pad_defaults(Pad *p)
{
    memset(p, 0, sizeof *p);
    p->sample = -1;
    p->level = 0.8f;
    p->cutoff = 1.0f;
}

static void reset_state(Engine *e)
{
    for (int i = 0; i < NUM_PADS; i++)
        pad_defaults(&e->pads[i]);
    for (int s = 0; s < MAX_SEQS; s++) {
        e->seqs[s].bars = 2;
        e->seqs[s].quant = 3;
        e->seqs[s].nev = 0;
    }
    e->cur_seq = e->next_seq = 0;
    e->bpm = 90;
    e->swing = 0;
    e->click = CLICK_REC;
    e->count_in = 1;
    e->song.n = 1;
    e->song.seq[0] = 0;
    e->song.reps[0] = 1;
    e->song_mode = 0;
    e->song_pos = 0;
    e->song_rep = 0;
    fx_defaults(&e->fx);
}

int engine_init(Engine *e)
{
    eng = e;
    reset_state(e);
    e->out_rate = 44100;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512; /* ~12 ms: playable pad latency */
    want.callback = audio_cb;
    if (SDL_OpenAudio(&want, &have) < 0) {
        fx_init(e->out_rate);
        return -1;
    }
    if (have.format != AUDIO_S16SYS || have.channels != 2) {
        SDL_CloseAudio();
        fx_init(e->out_rate);
        return -1;
    }
    e->out_rate = have.freq;
    fx_init(e->out_rate);
    SDL_PauseAudio(0);
    return 0;
}

void engine_quit(Engine *e)
{
    SDL_CloseAudio();
    for (int i = 0; i < MAX_SAMPLES; i++)
        sample_free(&e->pool[i]);
    fx_quit();
}

static void kill_voices_using(const Sample *s)
{
    for (int i = 0; i <= MAX_VOICES; i++)
        if (voices[i].active && voices[i].s == s)
            voices[i].active = 0;
}

void engine_reset(Engine *e)
{
    SDL_LockAudio();
    e->playing = 0;
    for (int i = 0; i <= MAX_VOICES; i++)
        voices[i].active = 0;
    fx_clear();
    SDL_UnlockAudio();
    for (int i = 0; i < MAX_SAMPLES; i++)
        sample_free(&e->pool[i]);
    reset_state(e);
}

int engine_load_sample(Engine *e, const char *path)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (e->pool[i].data && !strcmp(e->pool[i].path, path))
            return i;
        if (!e->pool[i].data && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        engine_gc(e);
        for (int i = 0; i < MAX_SAMPLES; i++)
            if (!e->pool[i].data) {
                free_slot = i;
                break;
            }
    }
    if (free_slot < 0)
        return -1;
    Sample tmp;
    if (sample_load_wav(&tmp, path) != 0)
        return -1;
    e->pool[free_slot] = tmp; /* slot unused, no voice points here */
    return free_slot;
}

Sample *engine_pad_sample(Engine *e, int pad)
{
    int s = e->pads[pad].sample;
    return (s >= 0 && e->pool[s].data) ? &e->pool[s] : NULL;
}

void engine_assign(Engine *e, int pad, int sample)
{
    SDL_LockAudio();
    Pad *p = &e->pads[pad];
    p->sample = sample;
    p->start = 0;
    p->end = sample >= 0 ? e->pool[sample].frames : 0;
    SDL_UnlockAudio();
    snprintf(p->label, sizeof p->label, "%s", sample >= 0 ? e->pool[sample].name : "");
    engine_gc(e);
}

void engine_gc(Engine *e)
{
    for (int i = 0; i < MAX_SAMPLES; i++) {
        if (!e->pool[i].data)
            continue;
        int used = 0;
        for (int p = 0; p < NUM_PADS; p++)
            if (e->pads[p].sample == i)
                used = 1;
        if (!used) {
            SDL_LockAudio();
            kill_voices_using(&e->pool[i]);
            SDL_UnlockAudio();
            sample_free(&e->pool[i]);
        }
    }
}

void engine_trigger(Engine *e, int pad, int velocity) { engine_trigger_semi(e, pad, velocity, 0); }

void engine_trigger_semi(Engine *e, int pad, int velocity, int semi)
{
    SDL_LockAudio();
    start_voice(pad, velocity, semi);
    SDL_UnlockAudio();
}

double engine_preview_pos(void)
{
    Voice *v = &voices[MAX_VOICES];
    return v->active ? v->pos : -1;
}

void engine_preview(Engine *e, const Sample *s, uint32_t start, uint32_t end)
{
    SDL_LockAudio();
    Voice *v = &voices[MAX_VOICES];
    v->active = s && s->data && end > start + 1;
    v->pad = -1;
    v->s = s;
    v->pos = start;
    v->end = s && end < s->frames ? end : (s ? s->frames : 0);
    v->step = s ? (double)s->rate / e->out_rate : 1;
    SDL_UnlockAudio();
}

void engine_stop_preview(Engine *e)
{
    SDL_LockAudio();
    voices[MAX_VOICES].active = 0;
    SDL_UnlockAudio();
}

static void start_locked(Engine *e, int on, int count_in)
{
    e->playing = on;
    if (on && e->song_mode && e->song.n) {
        e->song_pos = 0;
        e->song_rep = 0;
        e->next_seq = e->song.seq[0] % MAX_SEQS;
    }
    e->cur_seq = e->next_seq;
    real_tick = -1e-9; /* first frame lands on tick 0 and fires it */
    play_idx = 0;
    last_beat = -1;
    e->play_tick = 0;
    if (on && count_in) {
        real_tick = -4.0 * PPQ;
        e->play_tick = real_tick;
    }
    Sequence *sq = &e->seqs[e->cur_seq];
    for (int i = 0; i < sq->nev; i++)
        sq->ev[i].flags &= ~EV_SKIP;
}

void engine_set_playing(Engine *e, int on)
{
    SDL_LockAudio();
    start_locked(e, on, 0);
    SDL_UnlockAudio();
}

void engine_start_recording(Engine *e)
{
    SDL_LockAudio();
    e->rec = 1;
    if (!e->playing)
        start_locked(e, 1, e->count_in);
    SDL_UnlockAudio();
}

void engine_resync(Engine *e)
{
    SDL_LockAudio();
    Sequence *sq = &e->seqs[e->cur_seq];
    play_idx = 0;
    while (play_idx < sq->nev && sq->ev[play_idx].tick <= e->play_tick)
        play_idx++;
    SDL_UnlockAudio();
}

int engine_render(Engine *e, int seq, float tail_sec, int16_t **out)
{
    SDL_LockAudio();
    /* save live state */
    static Voice saved[MAX_VOICES + 1];
    memcpy(saved, voices, sizeof voices);
    int sp = e->playing, scur = e->cur_seq, snext = e->next_seq, ssong = e->song_mode, srec = e->rec;
    double stick = real_tick, splay = e->play_tick;
    int sidx = play_idx, sbeat = last_beat;
    uint32_t smask = e->repeat_mask;

    for (int i = 0; i <= MAX_VOICES; i++)
        voices[i].active = 0;
    fx_clear();
    e->song_mode = 0;
    e->rec = 0;
    e->repeat_mask = 0;
    e->next_seq = seq;
    rendering = 1;
    start_locked(e, 1, 0);
    double secs = seq_len(&e->seqs[seq]) * 60.0 / (PPQ * (double)e->bpm);
    if (secs > 120)
        secs = 120;
    int body = (int)(secs * e->out_rate), total = body + (int)(tail_sec * e->out_rate);
    int16_t *buf = malloc(sizeof(int16_t) * 2 * (size_t)total);
    if (buf) {
        int done = 0;
        while (done < total) {
            int n = total - done > 512 ? 512 : total - done;
            if (done + n > body && e->playing) {
                n = body - done;
                if (n <= 0) {
                    e->playing = 0;
                    continue;
                }
            }
            render(buf + done * 2, n);
            done += n;
            if (done >= body)
                e->playing = 0;
        }
    }
    rendering = 0;
    /* restore */
    fx_clear();
    memcpy(voices, saved, sizeof voices);
    for (int i = 0; i <= MAX_VOICES; i++)
        voices[i].active = 0;
    e->playing = 0;
    (void)sp;
    e->cur_seq = scur;
    e->next_seq = snext;
    e->song_mode = ssong;
    e->rec = srec;
    real_tick = stick;
    e->play_tick = splay;
    play_idx = sidx;
    last_beat = sbeat;
    e->repeat_mask = smask;
    e->master_meter[0] = e->master_meter[1] = 0;
    SDL_UnlockAudio();
    *out = buf;
    return buf ? total : -1;
}

/* ---------------- sequence editing ---------------- */

static int insert_locked(Engine *e, int si, uint32_t tick, int pad, int vel, int semi, int flags)
{
    Sequence *s = &e->seqs[si];
    if (s->nev >= MAX_EVENTS || tick >= seq_len(s))
        return -1;
    int pos = s->nev;
    while (pos > 0 && s->ev[pos - 1].tick > tick)
        pos--;
    memmove(&s->ev[pos + 1], &s->ev[pos], sizeof(Event) * (s->nev - pos));
    s->ev[pos] = (Event){tick, (uint8_t)pad, (uint8_t)vel, (uint8_t)flags, (int8_t)semi};
    s->nev++;
    if (si == e->cur_seq && e->playing) {
        /* keep the playback index pointing at the same next event */
        if (tick <= e->play_tick) {
            if (pos <= play_idx)
                play_idx++;
        }
        else if (pos < play_idx)
            play_idx++;
    }
    return pos;
}

int seq_add_semi(Engine *e, int seq, uint32_t tick, int pad, int vel, int semi)
{
    SDL_LockAudio();
    int r = insert_locked(e, seq, tick, pad, vel, semi, 0);
    SDL_UnlockAudio();
    return r;
}

int seq_add(Engine *e, int seq, uint32_t tick, int pad, int vel)
{
    SDL_LockAudio();
    int r = insert_locked(e, seq, tick, pad, vel, 0, 0);
    SDL_UnlockAudio();
    return r;
}

int engine_record_hit(Engine *e, int pad, int velocity, int semi)
{
    SDL_LockAudio();
    Sequence *s = &e->seqs[e->cur_seq];
    double t = e->play_tick;
    if (t < 0)
        t = 0; /* hit during count-in lands on the downbeat */
    int q = quant_ticks[s->quant];
    uint32_t len = seq_len(s);
    uint32_t tick = q ? (uint32_t)(floor(t / q + 0.5) * q) : (uint32_t)t;
    if (tick >= len)
        tick -= len;
    /* already heard live: if quantized ahead of the playhead, skip it this pass */
    int flags = (double)tick > t && tick != 0 ? EV_SKIP : 0;
    /* replace an existing hit on the same pad and tick (no doubles) */
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].tick == tick && s->ev[i].pad == pad && s->ev[i].semi == semi) {
            s->ev[i].vel = velocity;
            SDL_UnlockAudio();
            return i;
        }
    int r = insert_locked(e, e->cur_seq, tick, pad, velocity, semi, flags);
    SDL_UnlockAudio();
    return r;
}

void seq_remove_range(Engine *e, int si, int pad, uint32_t t0, uint32_t t1)
{
    SDL_LockAudio();
    Sequence *s = &e->seqs[si];
    int w = 0;
    for (int i = 0; i < s->nev; i++) {
        Event *ev = &s->ev[i];
        int kill = ev->tick >= t0 && ev->tick < t1 && (pad < 0 || ev->pad == pad);
        if (kill) {
            if (si == e->cur_seq && i < play_idx)
                play_idx--;
            continue;
        }
        s->ev[w++] = *ev;
    }
    s->nev = w;
    if (play_idx < 0)
        play_idx = 0;
    SDL_UnlockAudio();
}

int seq_count_range(const Engine *e, int si, int pad, uint32_t t0, uint32_t t1)
{
    const Sequence *s = &e->seqs[si];
    int n = 0;
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].tick >= t0 && s->ev[i].tick < t1 && (pad < 0 || s->ev[i].pad == pad))
            n++;
    return n;
}

void seq_set_vel_range(Engine *e, int si, int pad, uint32_t t0, uint32_t t1, int vel)
{
    SDL_LockAudio();
    Sequence *s = &e->seqs[si];
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].tick >= t0 && s->ev[i].tick < t1 && s->ev[i].pad == pad)
            s->ev[i].vel = vel;
    SDL_UnlockAudio();
}

void seq_set_bars(Engine *e, int si, int bars)
{
    if (bars < 1)
        bars = 1;
    if (bars > MAX_BARS)
        bars = MAX_BARS;
    Sequence *s = &e->seqs[si];
    if (bars < s->bars)
        seq_remove_range(e, si, -1, (uint32_t)bars * 4 * PPQ, 0xffffffffu); /* drop events past the end */
    SDL_LockAudio();
    s->bars = bars;
    SDL_UnlockAudio();
}

void seq_copy(Engine *e, int src, int dst)
{
    SDL_LockAudio();
    e->seqs[dst] = e->seqs[src];
    if (dst == e->cur_seq) { /* resync playback index to the playhead */
        play_idx = 0;
        while (play_idx < e->seqs[dst].nev && e->seqs[dst].ev[play_idx].tick <= e->play_tick)
            play_idx++;
    }
    SDL_UnlockAudio();
}

void engine_select_seq(Engine *e, int seq)
{
    SDL_LockAudio();
    e->next_seq = seq;
    if (!e->playing)
        e->cur_seq = seq;
    SDL_UnlockAudio();
}

void engine_lock(void) { SDL_LockAudio(); }
void engine_unlock(void) { SDL_UnlockAudio(); }

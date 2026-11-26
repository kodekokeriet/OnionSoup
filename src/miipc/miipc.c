// MiiPC: a small MPC-style drum machine for OnionSoup (Miyoo Mini / Mini+).
// 16 pads, 8 patterns of 16 steps, tempo, swing, metronome, pattern chain
// and live recording with quantize. Comes with a synthesized kit; own kits are
// folders of WAV files in Media/MiiPC/Kits/<name> (first 16 files, sorted).
//
// Keys: D-pad pad/step, A play pad / set step, B mute pad, X rec (pads) or
// accent (steps), Y pads/steps view, START play/stop, SELECT menu,
// L1/R1 pattern, L2/R2 tempo, MENU save and quit.

#include "soup/soup_ui.h"
#include <dirent.h>
#include <signal.h>
#include <strings.h>

#define NPADS 16
#define NSTEPS 16
#define NPAT 8
#define MAX_KITS 32

#define K_A SDLK_SPACE
#define K_B SDLK_LCTRL
#define K_X SDLK_LSHIFT
#define K_Y SDLK_LALT
#define K_L1 SDLK_e
#define K_R1 SDLK_t
#define K_L2 SDLK_TAB
#define K_R2 SDLK_BACKSPACE
#define K_SELECT SDLK_RCTRL
#define K_START SDLK_RETURN
#define K_MENU SDLK_ESCAPE

typedef struct {
    Sint16 *data;
    int len;
    int rate;
    char name[40];
} Sample;

typedef struct {
    int vol;   // 0..100
    int pitch; // semitones -12..12
    bool mute;
} PadCfg;

typedef struct {
    const Sint16 *data;
    int len;
    float pos, inc, gain;
    bool on;
} Voice;

static Sample samples[NPADS], click;
static PadCfg pads[NPADS];
static Uint8 pat[NPAT][NPADS][NSTEPS]; // 0 off, 1 hit, 2 accent
static Voice voices[NPADS + 1];        // last one is the metronome

// shared with the audio thread (guarded by SDL_LockAudio)
static int out_rate = 44100;
static bool audio_ok = false;
static volatile bool playing = false;
static volatile int cur_step = 0, cur_pat = 0, next_pat = 0;
static double step_left = 0;
static bool skip_next[NPADS];
static volatile Uint8 hit[NPADS];

static int bpm = 96, swing = 50, chain = 1;
static bool rec = false, metronome = true;
static char kit_name[64] = "Innebygd";
static bool builtin_kit = true;

static char kits[MAX_KITS][64];
static int n_kits = 0;

static char sdcard[256], kits_dir[300], state_dir[300], project_path[400], ui_font[400];

static int view = 0; // 0 pads, 1 steps
static int sel = 0;  // selected pad
static int step_cur = 0;
static bool menu = false;
static int menu_sel = 0;
static bool dirty = true;
static volatile sig_atomic_t quit = 0;
static char message[80];
static Uint32 message_until = 0;
static Uint32 flash_until[NPADS];

static void on_signal(int s)
{
    (void)s;
    quit = 1;
}

static void say(const char *m)
{
    copy_str(message, sizeof(message), m);
    message_until = SDL_GetTicks() + 1500;
    dirty = true;
}

static void mkdir_p(const char *path)
{
    char tmp[400];
    copy_str(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ---------- built-in kit ---------- */

static const char *builtin_names[NPADS] = {
    "Kick", "Snare", "Clap", "Hat", "Open hat", "Tom lav", "Tom mid", "Tom høy",
    "Rim", "Cowbell", "Crash", "Shaker", "Bass C", "Bass D#", "Bass G", "Bass A#"};

static Uint32 rng = 22222;
static float noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 8388608.0f - 1.0f;
}

static void finish_sample(Sample *s, float *buf, int n, const char *name)
{
    float peak = 0.0001f;
    for (int i = 0; i < n; i++)
        if (fabsf(buf[i]) > peak)
            peak = fabsf(buf[i]);
    s->data = malloc(n * sizeof(Sint16));
    for (int i = 0; i < n; i++)
        s->data[i] = (Sint16)(buf[i] / peak * 0.9f * 32767.0f);
    s->len = n;
    s->rate = out_rate;
    copy_str(s->name, sizeof(s->name), name);
    free(buf);
}

static void synth(int kind, Sample *s)
{
    static const float durs[] = {0.5f, 0.3f, 0.35f, 0.08f, 0.45f, 0.45f, 0.4f, 0.35f,
                                 0.06f, 0.3f, 1.5f, 0.12f, 1.0f, 1.0f, 1.0f, 1.0f, 0.03f};
    static const float bass[] = {65.41f, 77.78f, 98.0f, 116.54f};
    float R = (float)out_rate;
    int n = (int)(durs[kind] * R);
    float *b = calloc(n, sizeof(float));
    float ph = 0, ph2 = 0, prev = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float t = i / R, v = 0, nz = noise();
        float hp = nz - prev; // first difference: bright noise
        prev = nz;
        switch (kind) {
        case 0: // kick
            ph += 2 * M_PI * (45 + 110 * expf(-t * 30)) / R;
            v = sinf(ph) * expf(-t * 7) + (t < 0.002f ? nz * 0.3f : 0);
            break;
        case 1: // snare
            v = sinf(2 * M_PI * 185 * t) * expf(-t * 20) * 0.5f + hp * expf(-t * 14) * 0.6f;
            break;
        case 2: { // clap
            float env = t < 0.03f ? expf(-fmodf(t, 0.01f) * 300) : expf(-(t - 0.03f) * 12);
            lp += (nz - lp) * 0.35f;
            v = (nz - lp) * env;
            break;
        }
        case 3: // closed hat
            v = hp * expf(-t * 60);
            break;
        case 4: // open hat
            v = hp * expf(-t * 8);
            break;
        case 5:
        case 6:
        case 7: { // toms
            float base = kind == 5 ? 90 : kind == 6 ? 130 : 180;
            ph += 2 * M_PI * base * (1 + 0.5f * expf(-t * 15)) / R;
            v = sinf(ph) * expf(-t * 9);
            break;
        }
        case 8: // rim
            v = sinf(2 * M_PI * 1700 * t) * expf(-t * 90) + hp * expf(-t * 120) * 0.5f;
            break;
        case 9: { // cowbell
            float sq = (fmodf(t * 540, 1) < 0.5f ? 1 : -1) + (fmodf(t * 800, 1) < 0.5f ? 1 : -1);
            lp += (sq - lp) * 0.3f;
            v = lp * expf(-t * 12);
            break;
        }
        case 10: // crash
            v = hp * expf(-t * 3) * 0.6f;
            break;
        case 11: // shaker
            v = hp * (t < 0.02f ? t / 0.02f : expf(-(t - 0.02f) * 30));
            break;
        case 16: // metronome click
            v = sinf(2 * M_PI * 1500 * t) * expf(-t * 80);
            break;
        default: { // 808 bass
            float f = bass[kind - 12];
            ph += 2 * M_PI * f * (1 + 0.3f * expf(-t * 40)) / R;
            ph2 += 2 * M_PI * f * 2 / R;
            v = tanhf(1.6f * (sinf(ph) + 0.15f * sinf(ph2))) * expf(-t * 2.5f);
            if (t > durs[kind] - 0.02f)
                v *= (durs[kind] - t) / 0.02f;
            break;
        }
        }
        b[i] = v;
    }
    finish_sample(s, b, n, kind < NPADS ? builtin_names[kind] : "Klikk");
}

/* ---------- kits from WAV files ---------- */

static bool is_wav(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".wav") && n[0] != '.';
}

static int cmp_str(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void scan_kits(void)
{
    n_kits = 0;
    copy_str(kits[n_kits++], 64, "Innebygd");
    DIR *d = opendir(kits_dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && n_kits < MAX_KITS) {
        if (e->d_name[0] == '.')
            continue;
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", kits_dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            copy_str(kits[n_kits++], 64, e->d_name);
    }
    closedir(d);
    qsort(kits + 1, n_kits - 1, 64, cmp_str);
}

static bool load_wav(const char *path, Sample *s)
{
    SDL_AudioSpec spec;
    Uint8 *buf;
    Uint32 len;
    if (!SDL_LoadWAV(path, &spec, &buf, &len))
        return false;
    SDL_AudioCVT cvt;
    int r = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 1, spec.freq);
    if (r < 0) {
        SDL_FreeWAV(buf);
        return false;
    }
    Uint8 *data = malloc(len * (r ? cvt.len_mult : 1));
    memcpy(data, buf, len);
    SDL_FreeWAV(buf);
    int out_len = len;
    if (r) {
        cvt.buf = data;
        cvt.len = len;
        SDL_ConvertAudio(&cvt);
        out_len = cvt.len_cvt;
    }
    s->data = (Sint16 *)data;
    s->len = out_len / 2;
    s->rate = spec.freq;
    return s->len > 1;
}

static void free_samples(void)
{
    for (int i = 0; i < NPADS; i++) {
        free(samples[i].data);
        memset(&samples[i], 0, sizeof(Sample));
    }
}

static void load_kit(const char *name)
{
    SDL_LockAudio();
    for (int i = 0; i < NPADS + 1; i++)
        voices[i].on = false;
    free_samples();
    builtin_kit = true;
    if (strcmp(name, "Innebygd") != 0) {
        char dir[400];
        snprintf(dir, sizeof(dir), "%s/%s", kits_dir, name);
        static char files[256][128];
        int n = 0;
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < 256)
                if (is_wav(e->d_name))
                    copy_str(files[n++], 128, e->d_name);
            closedir(d);
        }
        if (n > 0) {
            qsort(files, n, 128, cmp_str);
            builtin_kit = false;
            for (int i = 0; i < NPADS; i++) {
                if (i >= n)
                    continue;
                char p[600];
                snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
                if (load_wav(p, &samples[i])) {
                    copy_str(samples[i].name, sizeof(samples[i].name), files[i]);
                    samples[i].name[strlen(samples[i].name) - 4] = '\0';
                }
                else {
                    free(samples[i].data);
                    memset(&samples[i], 0, sizeof(Sample));
                }
            }
        }
    }
    if (builtin_kit) {
        for (int i = 0; i < NPADS; i++)
            synth(i, &samples[i]);
        name = "Innebygd";
    }
    copy_str(kit_name, sizeof(kit_name), name);
    SDL_UnlockAudio();
}

/* ---------- audio ---------- */

// call with the audio locked (or from the callback)
static void trigger(int p, float g)
{
    if (p < NPADS) {
        hit[p] = 1;
        if (!samples[p].data)
            return;
        if (builtin_kit && p == 3)
            voices[4].on = false; // closed hat chokes the open hat
    }
    const Sample *s = p < NPADS ? &samples[p] : &click;
    Voice *v = &voices[p];
    v->data = s->data;
    v->len = s->len;
    v->pos = 0;
    v->inc = (float)s->rate / out_rate * (p < NPADS ? powf(2.0f, pads[p].pitch / 12.0f) : 1.0f);
    v->gain = g * (p < NPADS ? pads[p].vol / 100.0f : 1.0f);
    v->on = true;
}

static double step_len(int s)
{
    double base = out_rate * 60.0 / bpm / 4.0;
    double sw = (swing - 50) / 50.0; // 0..0.5
    return s % 2 == 0 ? base * (1 + sw) : base * (1 - sw);
}

static void fire_step(void)
{
    int s = cur_step;
    for (int p = 0; p < NPADS; p++) {
        Uint8 v = pat[cur_pat][p][s];
        if (skip_next[p]) {
            skip_next[p] = false;
            continue;
        }
        if (v && !pads[p].mute)
            trigger(p, v == 2 ? 1.0f : 0.7f);
    }
    if (rec && metronome && s % 4 == 0)
        trigger(NPADS, s == 0 ? 0.6f : 0.3f);
    step_left += step_len(s);
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    Sint16 *out = (Sint16 *)stream;
    int frames = len / 4;
    for (int f = 0; f < frames; f++) {
        if (playing) {
            step_left -= 1;
            if (step_left <= 0) {
                if (++cur_step >= NSTEPS) {
                    cur_step = 0;
                    if (next_pat != cur_pat)
                        cur_pat = next_pat;
                    else if (chain > 1)
                        cur_pat = next_pat = cur_pat + 1 < chain ? cur_pat + 1 : 0;
                }
                fire_step();
            }
        }
        float acc = 0;
        for (int i = 0; i < NPADS + 1; i++) {
            Voice *v = &voices[i];
            if (!v->on)
                continue;
            int idx = (int)v->pos;
            if (idx >= v->len - 1) {
                v->on = false;
                continue;
            }
            float a = v->data[idx], b = v->data[idx + 1];
            acc += (a + (b - a) * (v->pos - idx)) * v->gain;
            v->pos += v->inc;
        }
        acc *= 0.8f;
        if (acc > 32767)
            acc = 32767;
        if (acc < -32768)
            acc = -32768;
        out[f * 2] = out[f * 2 + 1] = (Sint16)acc;
    }
}

static void open_audio(void)
{
    SDL_AudioSpec want = {0}, got;
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_cb;
    if (SDL_OpenAudio(&want, &got) == 0 && got.format == AUDIO_S16SYS && got.channels == 2) {
        out_rate = got.freq;
        audio_ok = true;
    }
    else
        fprintf(stderr, "miipc: no audio: %s\n", SDL_GetError());
}

static void toggle_play(void)
{
    SDL_LockAudio();
    playing = !playing;
    if (playing) {
        cur_step = 0;
        cur_pat = next_pat;
        step_left = 0;
        memset(skip_next, 0, sizeof(skip_next));
        fire_step();
    }
    SDL_UnlockAudio();
    dirty = true;
}

static void play_pad(int p)
{
    SDL_LockAudio();
    trigger(p, 0.85f);
    if (playing && rec) {
        // quantize to the nearest step
        double len = step_len(cur_step);
        int target = step_left > len / 2 ? cur_step : (cur_step + 1) % NSTEPS;
        pat[cur_pat][p][target] = 1;
        if (target != cur_step)
            skip_next[p] = true; // it is already sounding now
    }
    SDL_UnlockAudio();
}

/* ---------- project file ---------- */

static void save_project(void)
{
    mkdir_p(state_dir);
    FILE *fp = fopen(project_path, "w");
    if (!fp)
        return;
    fprintf(fp, "bpm %d\nswing %d\nchain %d\nmetronome %d\nkit %s\n", bpm, swing, chain, metronome, kit_name);
    for (int p = 0; p < NPADS; p++)
        fprintf(fp, "pad %d %d %d %d\n", p, pads[p].vol, pads[p].pitch, pads[p].mute);
    for (int q = 0; q < NPAT; q++)
        for (int p = 0; p < NPADS; p++) {
            char row[NSTEPS + 1];
            bool any = false;
            for (int s = 0; s < NSTEPS; s++) {
                row[s] = ".xX"[pat[q][p][s]];
                any |= pat[q][p][s] != 0;
            }
            row[NSTEPS] = '\0';
            if (any)
                fprintf(fp, "pat %d %d %s\n", q, p, row);
        }
    fclose(fp);
}

static void demo_pattern(void)
{
    const char *rows[] = {"x...x...x...x...", "....X.......X...", NULL, "x.x.x.x.x.x.x.xX"};
    for (int p = 0; p < 4; p++)
        for (int s = 0; rows[p] && s < NSTEPS; s++)
            pat[0][p][s] = rows[p][s] == 'X' ? 2 : rows[p][s] == 'x' ? 1 : 0;
    const char *b = "x.....x...x.....";
    for (int s = 0; s < NSTEPS; s++)
        pat[0][12][s] = b[s] == 'x';
}

static void load_project(void)
{
    for (int p = 0; p < NPADS; p++)
        pads[p] = (PadCfg){80, 0, false};
    char want_kit[64] = "Innebygd";
    FILE *fp = fopen(project_path, "r");
    if (!fp) {
        demo_pattern();
        load_kit(want_kit);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        int a, b, c, d;
        char row[64];
        if (sscanf(line, "bpm %d", &a) == 1)
            bpm = a < 40 ? 40 : a > 240 ? 240 : a;
        else if (sscanf(line, "swing %d", &a) == 1)
            swing = a < 50 ? 50 : a > 75 ? 75 : a;
        else if (sscanf(line, "chain %d", &a) == 1)
            chain = a < 1 ? 1 : a > NPAT ? NPAT : a;
        else if (sscanf(line, "metronome %d", &a) == 1)
            metronome = a;
        else if (!strncmp(line, "kit ", 4))
            copy_str(want_kit, sizeof(want_kit), line + 4);
        else if (sscanf(line, "pad %d %d %d %d", &a, &b, &c, &d) == 4 && a >= 0 && a < NPADS)
            pads[a] = (PadCfg){b, c, d != 0};
        else if (sscanf(line, "pat %d %d %63s", &a, &b, row) == 3 && a >= 0 && a < NPAT && b >= 0 && b < NPADS)
            for (int s = 0; s < NSTEPS && row[s]; s++)
                pat[a][b][s] = row[s] == 'X' ? 2 : row[s] == 'x' ? 1 : 0;
    }
    fclose(fp);
    load_kit(want_kit);
}

/* ---------- menu ---------- */

enum { M_KIT, M_BPM, M_SWING, M_CHAIN, M_METRO, M_VOL, M_PITCH, M_COPY, M_CLEAR, M_SAVE, M_COUNT };

static void menu_value(int i, char *out, size_t n)
{
    switch (i) {
    case M_KIT:
        copy_str(out, n, kit_name);
        break;
    case M_BPM:
        snprintf(out, n, "%d BPM", bpm);
        break;
    case M_SWING:
        snprintf(out, n, "%d%%", swing);
        break;
    case M_CHAIN:
        if (chain > 1)
            snprintf(out, n, "Mønster 1-%d", chain);
        else
            copy_str(out, n, "Av");
        break;
    case M_METRO:
        copy_str(out, n, metronome ? "På ved opptak" : "Av");
        break;
    case M_VOL:
        snprintf(out, n, "%d", pads[sel].vol);
        break;
    case M_PITCH:
        snprintf(out, n, "%+d", pads[sel].pitch);
        break;
    default:
        out[0] = '\0';
    }
}

static const char *menu_label(int i)
{
    static char buf[64];
    switch (i) {
    case M_KIT:
        return "Kit";
    case M_BPM:
        return "Tempo";
    case M_SWING:
        return "Swing";
    case M_CHAIN:
        return "Kjede";
    case M_METRO:
        return "Metronom";
    case M_VOL:
        snprintf(buf, sizeof(buf), "Volum %s", samples[sel].name[0] ? samples[sel].name : "pad");
        return buf;
    case M_PITCH:
        return "Tonehøyde";
    case M_COPY:
        return "Kopier mønster til neste";
    case M_CLEAR:
        return "Tøm mønster";
    default:
        return "Lagre nå";
    }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void menu_change(int d)
{
    switch (menu_sel) {
    case M_KIT: {
        int k = 0;
        for (int i = 0; i < n_kits; i++)
            if (!strcmp(kits[i], kit_name))
                k = i;
        k = (k + d + n_kits) % n_kits;
        load_kit(kits[k]);
        break;
    }
    case M_BPM:
        bpm = clampi(bpm + d, 40, 240);
        break;
    case M_SWING:
        swing = clampi(swing + d, 50, 75);
        break;
    case M_CHAIN:
        chain = clampi(chain + d, 1, NPAT);
        break;
    case M_METRO:
        metronome = !metronome;
        break;
    case M_VOL:
        pads[sel].vol = clampi(pads[sel].vol + d * 5, 0, 100);
        break;
    case M_PITCH:
        pads[sel].pitch = clampi(pads[sel].pitch + d, -12, 12);
        if (pads[sel].pitch != 0 || d)
            play_pad(sel);
        break;
    }
}

static void menu_action(void)
{
    int q = next_pat;
    switch (menu_sel) {
    case M_COPY:
        if (q + 1 < NPAT) {
            memcpy(pat[q + 1], pat[q], sizeof(pat[q]));
            next_pat = q + 1;
            if (!playing)
                cur_pat = next_pat;
            say("Kopiert til neste mønster");
        }
        break;
    case M_CLEAR:
        memset(pat[q], 0, sizeof(pat[q]));
        say("Mønsteret er tømt");
        break;
    case M_SAVE:
        save_project();
        say("Lagret");
        break;
    default:
        menu_change(1);
    }
}

/* ---------- drawing ---------- */

static int pad_at(int r, int c) { return (3 - r) * 4 + c; } // pad 1 bottom left, like an MPC

static bool pad_has_steps(int p)
{
    for (int s = 0; s < NSTEPS; s++)
        if (pat[next_pat][p][s])
            return true;
    return false;
}

static void draw_header(void)
{
    soup_title("Mii", "PC");
    char buf[64];
    int x = SCREEN_W - 16;
    const char *st = rec ? "REC" : playing ? "SPILLER" : "STOPP";
    SDL_Color pc = rec ? C_RED : playing ? C_GREEN : C_CARD;
    int w = text_w(f15, st) + 20;
    x -= w;
    fill_round(x, 15, w, 24, 12, pc);
    text(f15, st, x + 10, 17, rec || playing ? C_DARK : C_SOFT);
    snprintf(buf, sizeof(buf), "%d BPM", bpm);
    x -= text_w(f17, buf) + 16;
    text(f17, buf, x, 16, C_SOFT);
    if (next_pat != cur_pat && playing)
        snprintf(buf, sizeof(buf), "Mønster %d > %d", cur_pat + 1, next_pat + 1);
    else
        snprintf(buf, sizeof(buf), "Mønster %d", next_pat + 1);
    x -= text_w(f17, buf) + 16;
    text(f17, buf, x, 16, next_pat != cur_pat && playing ? C_ACCENT : C_SOFT);
}

static void draw_pads(Uint32 now)
{
    const int S = 88, G = 8, X0 = 16, Y0 = 62;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int p = pad_at(r, c);
            int x = X0 + c * (S + G), y = Y0 + r * (S + G);
            bool lit = now < flash_until[p];
            SDL_Color bg = lit ? C_ACCENT : C_CARD;
            if (p == sel)
                outline_round(x, y, S, S, 12, 3, C_WHITE, bg);
            else
                fill_round(x, y, S, S, 12, bg);
            char num[8];
            snprintf(num, sizeof(num), "%d", p + 1);
            SDL_Color fg = lit ? C_DARK : C_GREY;
            text(f13, num, x + 10, y + 7, fg);
            if (pad_has_steps(p))
                fill_round(x + S - 18, y + 10, 8, 8, 4, lit ? C_DARK : C_ACCENT);
            if (pads[p].mute)
                text(f13, "DEMPET", x + 10, y + 26, lit ? C_DARK : C_RED);
            const char *nm = samples[p].data ? samples[p].name : "—";
            text_fit(f15, nm, x + 10, y + S - 26, S - 18, lit ? C_DARK : pads[p].mute ? C_GREY : C_WHITE);
        }

    // info panel for the selected pad
    const int PX = 408, PW = 216;
    fill_round(PX, 62, PW, 376, 14, C_PANEL);
    char buf[80];
    snprintf(buf, sizeof(buf), "Pad %d", sel + 1);
    text(f15, buf, PX + 16, 74, C_GREY);
    text_fit(f24, samples[sel].data ? samples[sel].name : "Tom pad", PX + 16, 92, PW - 32, C_ACCENT);

    text(f15, "Volum", PX + 16, 136, C_GREY);
    fill_round(PX + 16, 158, PW - 32, 8, 4, C_CARD);
    fill_round(PX + 16, 158, (PW - 32) * pads[sel].vol / 100 < 8 ? 8 : (PW - 32) * pads[sel].vol / 100, 8, 4, C_ACCENT);
    snprintf(buf, sizeof(buf), "Tonehøyde %+d", pads[sel].pitch);
    text(f15, buf, PX + 16, 176, C_SOFT);
    if (pads[sel].mute)
        text(f15, "Dempet", PX + 140, 176, C_RED);

    snprintf(buf, sizeof(buf), "Mønster %d", next_pat + 1);
    text(f15, buf, PX + 16, 210, C_GREY);
    for (int s = 0; s < NSTEPS; s++) {
        int x = PX + 16 + (s % 8) * 23, y = 234 + (s / 8) * 28;
        Uint8 v = pat[next_pat][sel][s];
        SDL_Color c = v == 2 ? C_ACCENT : v == 1 ? C_SOFT : C_CARD;
        if (playing && s == cur_step && cur_pat == next_pat)
            outline_round(x, y, 19, 22, 5, 2, C_GREEN, c);
        else
            fill_round(x, y, 19, 22, 5, c);
    }

    snprintf(buf, sizeof(buf), "Kit: %s", kit_name);
    text_fit(f15, buf, PX + 16, 306, PW - 32, C_SOFT);
    snprintf(buf, sizeof(buf), "Swing %d%%", swing);
    text(f15, buf, PX + 16, 330, C_SOFT);
    if (chain > 1)
        snprintf(buf, sizeof(buf), "Kjede 1-%d", chain);
    else
        copy_str(buf, sizeof(buf), "Kjede av");
    text(f15, buf, PX + 16, 354, C_SOFT);
    if (!audio_ok)
        text(f15, "Ingen lyd funnet", PX + 16, 400, C_RED);
    else if (rec && !playing)
        text_wrap(f13, "Trykk START for å ta opp", PX + 16, 400, PW - 32, 16, 2, C_GREY);
}

static void draw_steps(void)
{
    const int Y0 = 58, RH = 24, NX = 16, SX = 116, CW = 31;
    if (playing && cur_pat == next_pat)
        fill_round(SX + cur_step * CW - 2, Y0 - 2, CW, RH * NPADS + 2, 6, C_PANEL);
    for (int p = 0; p < NPADS; p++) {
        int y = Y0 + p * RH;
        bool lit = SDL_GetTicks() < flash_until[p];
        SDL_Color nc = p == sel ? C_ACCENT : pads[p].mute ? C_GREY : C_SOFT;
        if (lit)
            fill_round(NX - 6, y, 4, RH - 4, 2, C_ACCENT);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d %s", p + 1, samples[p].data ? samples[p].name : "—");
        text_fit(f13, buf, NX, y + 2, SX - NX - 6, nc);
        for (int s = 0; s < NSTEPS; s++) {
            int x = SX + s * CW;
            Uint8 v = pat[next_pat][p][s];
            SDL_Color c = v == 2 ? C_ACCENT : v == 1 ? C_SOFT : ((s / 4) % 2 ? C_PANEL : C_CARD);
            if (p == sel && s == step_cur)
                outline_round(x, y, CW - 4, RH - 4, 5, 2, C_WHITE, c);
            else
                fill_round(x, y, CW - 4, RH - 4, 5, c);
        }
    }
}

static void draw_menu(void)
{
    const int W = 420, RH = 30, H = M_COUNT * RH + 56;
    int x = (SCREEN_W - W) / 2, y = (SCREEN_H - H) / 2 - 10;
    fill_round(x - 2, y - 2, W + 4, H + 4, 16, C_BAR);
    fill_round(x, y, W, H, 14, C_PANEL);
    text(f19, "Meny", x + 20, y + 12, C_WHITE);
    for (int i = 0; i < M_COUNT; i++) {
        int ry = y + 46 + i * RH;
        if (i == menu_sel)
            fill_round(x + 10, ry, W - 20, RH - 4, 8, C_ACCENT);
        SDL_Color fg = i == menu_sel ? C_DARK : C_SOFT;
        text_fit(f15, menu_label(i), x + 22, ry + 3, 220, fg);
        char v[80], shown[90];
        menu_value(i, v, sizeof(v));
        if (*v) {
            snprintf(shown, sizeof(shown), i == menu_sel ? "<  %s  >" : "%s", v);
            int w = text_w(f15, shown);
            if (w > 160)
                w = 160;
            text_fit(f15, shown, x + W - 22 - w, ry + 3, 160, fg);
        }
    }
}

static void draw(void)
{
    Uint32 now = SDL_GetTicks();
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    draw_header();
    if (view == 0)
        draw_pads(now);
    else
        draw_steps();
    if (menu) {
        draw_menu();
        const char *k[] = {"←→", "A", "B"}, *l[] = {"Endre", "Velg", "Lukk"};
        draw_hints(k, l, 3);
    }
    else if (view == 0) {
        const char *k[] = {"A", "X", "Y", "START", "SELECT"};
        const char *l[] = {"Spill", rec ? "Rec av" : "Rec", "Steg", playing ? "Stopp" : "Start", "Meny"};
        draw_hints(k, l, 5);
    }
    else {
        const char *k[] = {"A", "X", "B", "Y", "START"};
        const char *l[] = {"Sett", "Aksent", "Demp", "Pads", playing ? "Stopp" : "Start"};
        draw_hints(k, l, 5);
    }
    if (*message) {
        int w = text_w(f15, message) + 32;
        fill_round((SCREEN_W - w) / 2, 404, w, 32, 16, C_BAR);
        text(f15, message, (SCREEN_W - w) / 2 + 16, 410, C_WHITE);
    }
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
    dirty = false;
}

/* ---------- input ---------- */

static void set_pattern(int d)
{
    next_pat = (next_pat + d + NPAT) % NPAT;
    if (!playing)
        cur_pat = next_pat;
}

static void on_key(SDLKey k)
{
    dirty = true;
    if (k == K_MENU) {
        quit = 1;
        return;
    }
    if (k == K_START) {
        toggle_play();
        return;
    }
    if (k == K_L2 || k == K_R2) {
        bpm = clampi(bpm + (k == K_R2 ? 1 : -1), 40, 240);
        return;
    }
    if (menu) {
        if (k == SDLK_UP)
            menu_sel = (menu_sel + M_COUNT - 1) % M_COUNT;
        else if (k == SDLK_DOWN)
            menu_sel = (menu_sel + 1) % M_COUNT;
        else if (k == SDLK_LEFT || k == SDLK_RIGHT)
            menu_change(k == SDLK_RIGHT ? 1 : -1);
        else if (k == K_A)
            menu_action();
        else if (k == K_B || k == K_SELECT)
            menu = false;
        return;
    }
    switch (k) {
    case K_SELECT:
        scan_kits();
        menu = true;
        break;
    case K_L1:
    case K_R1:
        set_pattern(k == K_R1 ? 1 : -1);
        break;
    case K_Y:
        view = !view;
        break;
    case K_B:
        pads[sel].mute = !pads[sel].mute;
        break;
    default:
        break;
    }
    if (view == 0) {
        int r = 3 - sel / 4, c = sel % 4;
        if (k == SDLK_UP && r > 0)
            r--;
        else if (k == SDLK_DOWN && r < 3)
            r++;
        else if (k == SDLK_LEFT && c > 0)
            c--;
        else if (k == SDLK_RIGHT && c < 3)
            c++;
        sel = pad_at(r, c);
        if (k == K_A)
            play_pad(sel);
        else if (k == K_X)
            rec = !rec;
    }
    else {
        if (k == SDLK_UP)
            sel = (sel + NPADS - 1) % NPADS;
        else if (k == SDLK_DOWN)
            sel = (sel + 1) % NPADS;
        else if (k == SDLK_LEFT)
            step_cur = (step_cur + NSTEPS - 1) % NSTEPS;
        else if (k == SDLK_RIGHT)
            step_cur = (step_cur + 1) % NSTEPS;
        else if (k == K_A || k == K_X) {
            Uint8 *v = &pat[next_pat][sel][step_cur];
            if (k == K_A)
                *v = *v ? 0 : 1;
            else
                *v = *v == 2 ? 1 : 2;
            if (*v && !playing)
                play_pad(sel);
        }
    }
}

static bool repeats(SDLKey k)
{
    return k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT || k == K_L2 || k == K_R2;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    const char *e;
    copy_str(sdcard, sizeof(sdcard), (e = getenv("SDCARD")) ? e : "/mnt/SDCARD");
    snprintf(kits_dir, sizeof(kits_dir), "%s/Media/MiiPC/Kits", sdcard);
    snprintf(state_dir, sizeof(state_dir), "%s/Saves/CurrentProfile/states/MiiPC", sdcard);
    snprintf(project_path, sizeof(project_path), "%s/prosjekt.txt", state_dir);
    if ((e = getenv("MIIPC_FONT")))
        copy_str(ui_font, sizeof(ui_font), e);
    else
        snprintf(ui_font, sizeof(ui_font), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    mkdir_p(kits_dir);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 60);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!soup_open_fonts(ui_font)) {
        fprintf(stderr, "miipc: cannot open font %s\n", ui_font);
        return 1;
    }
    open_audio();
    synth(16, &click);
    scan_kits();
    load_project();
    if (audio_ok)
        SDL_PauseAudio(0);

    static bool held[SDLK_LAST];
    int last_step = -1, last_pat = -1;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = 1;
            else if (ev.type == SDL_KEYDOWN) {
                SDLKey k = ev.key.keysym.sym;
                if (k < SDLK_LAST && held[k] && !repeats(k))
                    continue;
                if (k < SDLK_LAST)
                    held[k] = true;
                on_key(k);
            }
            else if (ev.type == SDL_KEYUP && ev.key.keysym.sym < SDLK_LAST)
                held[ev.key.keysym.sym] = false;
        }
        Uint32 now = SDL_GetTicks();
        bool flashing = false;
        for (int p = 0; p < NPADS; p++) {
            if (hit[p]) {
                hit[p] = 0;
                flash_until[p] = now + 110;
                dirty = true;
            }
            if (flash_until[p] && now >= flash_until[p]) {
                flash_until[p] = 0;
                dirty = true;
            }
            flashing |= flash_until[p] != 0;
        }
        if (cur_step != last_step || cur_pat != last_pat) {
            last_step = cur_step;
            last_pat = cur_pat;
            dirty = true;
        }
        if (*message && now >= message_until) {
            message[0] = '\0';
            dirty = true;
        }
        if (dirty)
            draw();
        SDL_Delay(flashing || playing ? 8 : 25);
    }
    SDL_PauseAudio(1);
    save_project();
    if (audio_ok)
        SDL_CloseAudio();
    TTF_Quit();
    SDL_Quit();
    return 0;
}

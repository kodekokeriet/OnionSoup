#include "project.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Store paths relative to the app folder when possible, so the SD card can move. */
static const char *rel_path(const char *path, char *buf, int len)
{
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) {
        size_t n = strlen(cwd);
        if (!strncmp(path, cwd, n) && path[n] == '/') {
            snprintf(buf, len, "%s", path + n + 1);
            return buf;
        }
    }
    snprintf(buf, len, "%s", path);
    return buf;
}

int project_save(const Engine *e, const char *path, const char *name)
{
    mkdir(PROJECT_DIR, 0755);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    char rb[PATH_MAX];
    const FxParams *x = &e->fx;
    fprintf(f, "miipc 2\n");
    if (name && *name)
        fprintf(f, "name %s\n", name);
    fprintf(f, "song bpm=%d swing=%.3f click=%d seq=%d countin=%d songmode=%d\n",
            e->bpm, e->swing, e->click, e->next_seq, e->count_in, e->song_mode);
    fprintf(f, "chain");
    for (int i = 0; i < e->song.n; i++)
        fprintf(f, " %d:%d", e->song.seq[i], e->song.reps[i]);
    fprintf(f, "\n");
    fprintf(f, "fx dly_sync=%d dly_fb=%.3f dly_level=%.3f rev_size=%.3f rev_damp=%.3f rev_level=%.3f "
               "comp_thresh=%.1f comp_ratio=%.2f master=%.3f\n",
            x->dly_sync, x->dly_fb, x->dly_level, x->rev_size, x->rev_damp, x->rev_level,
            x->comp_thresh, x->comp_ratio, x->master);
    for (int i = 0; i < MAX_SAMPLES; i++)
        if (e->pool[i].data)
            fprintf(f, "sample %d %s\n", i, rel_path(e->pool[i].path, rb, sizeof rb));
    for (int p = 0; p < NUM_PADS; p++) {
        const Pad *d = &e->pads[p];
        fprintf(f, "pad %d sample=%d start=%u end=%u pitch=%d choke=%d attack=%.1f decay=%.1f "
                   "level=%.3f pan=%.3f mute=%d solo=%d filter=%d cutoff=%.3f reso=%.3f drive=%.3f "
                   "crush=%d sdly=%.3f srev=%.3f label=%s\n",
                p, d->sample, d->start, d->end, d->pitch, d->choke, d->attack, d->decay,
                d->level, d->pan, d->mute, d->solo, d->filter, d->cutoff, d->reso, d->drive,
                d->crush, d->send_dly, d->send_rev, d->label);
    }
    for (int s = 0; s < MAX_SEQS; s++) {
        const Sequence *q = &e->seqs[s];
        if (!q->nev && q->bars == 2 && q->quant == 3)
            continue;
        fprintf(f, "seq %d bars=%d quant=%d\n", s, q->bars, q->quant);
        for (int i = 0; i < q->nev; i++) {
            const Event *v = &q->ev[i];
            if (v->semi)
                fprintf(f, "ev %u %d %d %d\n", v->tick, v->pad, v->vel, v->semi);
            else
                fprintf(f, "ev %u %d %d\n", v->tick, v->pad, v->vel);
        }
    }
    int ok = fflush(f) == 0;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    sync();
    return 0;
}

/* key=value helpers: look for " key=" in the line. */
static const char *kv(const char *line, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = line; (p = strstr(p, key)); p++)
        if ((p == line || p[-1] == ' ') && p[n] == '=')
            return p + n + 1;
    return NULL;
}
static void kv_int(const char *l, const char *k, int *v)
{
    const char *s = kv(l, k);
    if (s)
        *v = atoi(s);
}
static void kv_u32(const char *l, const char *k, uint32_t *v)
{
    const char *s = kv(l, k);
    if (s)
        *v = (uint32_t)strtoul(s, NULL, 10);
}
static void kv_flt(const char *l, const char *k, float *v)
{
    const char *s = kv(l, k);
    if (s)
        *v = (float)atof(s);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi
                                                                       : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi
                                                                               : v; }

int project_load(Engine *e, const char *path, char *name_out, int name_len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    if (!fgets(line, sizeof line, f) || strncmp(line, "miipc 2", 7)) {
        fclose(f);
        return -1;
    }

    engine_reset(e);
    int map[MAX_SAMPLES];
    for (int i = 0; i < MAX_SAMPLES; i++)
        map[i] = -1;
    if (name_out)
        name_out[0] = 0;
    int cur = -1, seqsel = 0;

    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int i;
        char str[768];
        if (!strncmp(line, "name ", 5)) {
            if (name_out)
                snprintf(name_out, name_len, "%s", line + 5);
        }
        else if (!strncmp(line, "song ", 5)) {
            kv_int(line, "bpm", &e->bpm);
            kv_flt(line, "swing", &e->swing);
            kv_int(line, "click", &e->click);
            kv_int(line, "seq", &seqsel);
            kv_int(line, "countin", &e->count_in);
            kv_int(line, "songmode", &e->song_mode);
            e->count_in = !!e->count_in;
            e->song_mode = !!e->song_mode;
            e->bpm = clampi(e->bpm, 40, 240);
            e->swing = clampf(e->swing, 0, 0.5f);
            e->click = clampi(e->click, 0, NUM_CLICK - 1);
            seqsel = clampi(seqsel, 0, MAX_SEQS - 1);
        }
        else if (!strncmp(line, "chain", 5)) {
            const char *p = line + 5;
            int sq, rp, n;
            e->song.n = 0;
            while (e->song.n < MAX_SONG && sscanf(p, " %d:%d%n", &sq, &rp, &n) == 2) {
                e->song.seq[e->song.n] = (uint8_t)clampi(sq, 0, MAX_SEQS - 1);
                e->song.reps[e->song.n] = (uint8_t)clampi(rp, 1, 16);
                e->song.n++;
                p += n;
            }
        }
        else if (!strncmp(line, "fx ", 3)) {
            FxParams *x = &e->fx;
            kv_int(line, "dly_sync", &x->dly_sync);
            kv_flt(line, "dly_fb", &x->dly_fb);
            kv_flt(line, "dly_level", &x->dly_level);
            kv_flt(line, "rev_size", &x->rev_size);
            kv_flt(line, "rev_damp", &x->rev_damp);
            kv_flt(line, "rev_level", &x->rev_level);
            kv_flt(line, "comp_thresh", &x->comp_thresh);
            kv_flt(line, "comp_ratio", &x->comp_ratio);
            kv_flt(line, "master", &x->master);
            x->dly_sync = clampi(x->dly_sync, 0, NUM_DLY_SYNC - 1);
            x->dly_fb = clampf(x->dly_fb, 0, 0.95f);
            x->comp_ratio = clampf(x->comp_ratio, 1, 20);
        }
        else if (sscanf(line, "sample %d %767[^\n]", &i, str) == 2 && i >= 0 && i < MAX_SAMPLES) {
            map[i] = engine_load_sample(e, str);
            if (map[i] < 0)
                fprintf(stderr, "missing sample: %s\n", str);
        }
        else if (sscanf(line, "pad %d", &i) == 1 && i >= 0 && i < NUM_PADS) {
            Pad *d = &e->pads[i];
            int smp = -1;
            kv_int(line, "sample", &smp);
            d->sample = (smp >= 0 && smp < MAX_SAMPLES) ? map[smp] : -1;
            kv_u32(line, "start", &d->start);
            kv_u32(line, "end", &d->end);
            kv_int(line, "pitch", &d->pitch);
            kv_int(line, "choke", &d->choke);
            kv_flt(line, "attack", &d->attack);
            kv_flt(line, "decay", &d->decay);
            kv_flt(line, "level", &d->level);
            kv_flt(line, "pan", &d->pan);
            kv_int(line, "mute", &d->mute);
            kv_int(line, "solo", &d->solo);
            kv_int(line, "filter", &d->filter);
            kv_flt(line, "cutoff", &d->cutoff);
            kv_flt(line, "reso", &d->reso);
            kv_flt(line, "drive", &d->drive);
            kv_int(line, "crush", &d->crush);
            kv_flt(line, "sdly", &d->send_dly);
            kv_flt(line, "srev", &d->send_rev);
            const char *lb = kv(line, "label");
            snprintf(d->label, sizeof d->label, "%s", lb ? lb : "");
            uint32_t fr = d->sample >= 0 ? e->pool[d->sample].frames : 0;
            if (d->start >= fr)
                d->start = 0;
            if (d->end > fr || d->end <= d->start)
                d->end = fr;
            d->filter = clampi(d->filter, 0, NUM_FILT - 1);
            d->crush = clampi(d->crush, 0, NUM_CRUSH - 1);
            d->pitch = clampi(d->pitch, -12, 12);
        }
        else if (sscanf(line, "seq %d", &i) == 1 && i >= 0 && i < MAX_SEQS) {
            cur = i;
            Sequence *q = &e->seqs[i];
            kv_int(line, "bars", &q->bars);
            kv_int(line, "quant", &q->quant);
            q->bars = clampi(q->bars, 1, MAX_BARS);
            q->quant = clampi(q->quant, 0, NUM_QUANT - 1);
            q->nev = 0;
        }
        else if (!strncmp(line, "ev ", 3) && cur >= 0) {
            unsigned t;
            int p, v, semi = 0;
            if (sscanf(line, "ev %u %d %d %d", &t, &p, &v, &semi) >= 3 && p >= 0 && p < NUM_PADS)
                seq_add_semi(e, cur, t, p, clampi(v, 1, 127), clampi(semi, -24, 24));
        }
    }
    fclose(f);
    e->cur_seq = e->next_seq = seqsel;
    if (!e->song.n)
        e->song_mode = 0;
    engine_gc(e);
    return 0;
}

void project_next_path(char *out, int len)
{
    mkdir(PROJECT_DIR, 0755);
    for (int n = 1; n < 1000; n++) {
        snprintf(out, len, PROJECT_DIR "/project_%03d.mpj", n);
        if (access(out, F_OK) != 0)
            return;
    }
}

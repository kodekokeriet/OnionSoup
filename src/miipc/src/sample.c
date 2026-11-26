#include "sample.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Simple RIFF/WAVE reader: PCM 8/16/24-bit, mono/stereo. */
int sample_load_wav(Sample *s, const char *path)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = len > 0 ? malloc(len) : NULL;
    size_t got = 0;
    while (buf && got < (size_t)len) { /* read in chunks; fread may return short */
        size_t r = fread(buf + got, 1, len - got, f);
        if (r == 0)
            break;
        got += r;
    }
    fclose(f);
    if (!buf || got < 12) {
        free(buf);
        return -1;
    }
    len = (long)got;

    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        free(buf);
        return -1;
    }

    int fmt = 0, ch = 0, bits = 0, rate = 0;
    const uint8_t *pcm = NULL;
    uint32_t pcmlen = 0;
    long pos = 12;
    while (pos + 8 <= len) {
        uint32_t sz = rd32(buf + pos + 4);
        const uint8_t *d = buf + pos + 8;
        if (pos + 8 + (long)sz > len)
            sz = len - pos - 8;
        if (!memcmp(buf + pos, "fmt ", 4) && sz >= 16) {
            fmt = rd16(d);
            ch = rd16(d + 2);
            rate = rd32(d + 4);
            bits = rd16(d + 14);
        }
        else if (!memcmp(buf + pos, "data", 4)) {
            pcm = d;
            pcmlen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if ((fmt != 1 && fmt != 0xFFFE) || !pcm || ch < 1 || ch > 2 || rate <= 0 ||
        (bits != 8 && bits != 16 && bits != 24)) {
        free(buf);
        return -1;
    }

    int bps = bits / 8;
    s->frames = pcmlen / (bps * ch);
    s->channels = ch;
    s->rate = rate;
    if (s->frames < 2) {
        free(buf);
        return -1;
    }
    s->data = malloc((size_t)s->frames * ch * sizeof(int16_t));
    if (!s->data) {
        free(buf);
        return -1;
    }
    for (uint32_t i = 0; i < s->frames * ch; i++) {
        const uint8_t *p = pcm + (size_t)i * bps;
        int16_t v;
        if (bps == 1)
            v = (int16_t)((p[0] - 128) << 8);
        else if (bps == 2)
            v = (int16_t)rd16(p);
        else
            v = (int16_t)(p[1] | p[2] << 8);
        s->data[i] = v;
    }
    free(buf);

    snprintf(s->path, sizeof s->path, "%s", path);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(s->name, sizeof s->name, "%s", base);
    char *dot = strrchr(s->name, '.');
    if (dot)
        *dot = 0;
    return 0;
}

void sample_free(Sample *s)
{
    free(s->data);
    free(s->peaks);
    memset(s, 0, sizeof *s);
}

static inline int mono_at(const Sample *s, uint32_t i)
{
    return s->channels == 2 ? (s->data[i * 2] + s->data[i * 2 + 1]) / 2 : s->data[i];
}

const int16_t *sample_peaks(Sample *s, int w)
{
    if (!s->data || w <= 0)
        return NULL;
    if (s->peaks && s->peaks_w == w)
        return s->peaks;
    free(s->peaks);
    s->peaks = malloc(sizeof(int16_t) * 2 * w);
    s->peaks_w = w;
    for (int x = 0; x < w; x++) {
        uint32_t a = (uint64_t)s->frames * x / w, b = (uint64_t)s->frames * (x + 1) / w;
        if (b <= a)
            b = a + 1;
        int mn = 32767, mx = -32768;
        for (uint32_t i = a; i < b && i < s->frames; i++) {
            int v = mono_at(s, i);
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        s->peaks[x * 2] = mn;
        s->peaks[x * 2 + 1] = mx;
    }
    return s->peaks;
}

/* Energy-based onset detection: compares short-window energy against a
 * running average and picks the strongest peaks with a minimum spacing. */
int sample_detect_onsets(const Sample *s, uint32_t start, uint32_t end, uint32_t *out, int max)
{
    if (max < 1 || !s->data)
        return 0;
    if (end > s->frames)
        end = s->frames;
    out[0] = start;
    const uint32_t win = 256;
    if (end <= start + win * 4)
        return 1;
    int nw = (end - start) / win;
    float *e = malloc(sizeof(float) * nw);
    float *score = calloc(nw, sizeof(float));
    for (int w = 0; w < nw; w++) {
        double acc = 0;
        for (uint32_t i = 0; i < win; i++) {
            double v = mono_at(s, start + w * win + i);
            acc += v * v;
        }
        e[w] = (float)sqrt(acc / win);
    }
    float avg = e[0];
    for (int w = 1; w < nw; w++) {
        float rise = e[w] - avg;
        if (rise > 0 && e[w] > 300.0f && e[w] > avg * 1.6f)
            score[w] = rise;
        avg = avg * 0.85f + e[w] * 0.15f;
    }
    /* pick the strongest, at least ~60 ms apart */
    int minsep = (int)(s->rate * 0.06 / win) + 1, n = 1;
    while (n < max) {
        int best = -1;
        for (int w = 1; w < nw; w++)
            if (score[w] > 0 && (best < 0 || score[w] > score[best]))
                best = w;
        if (best < 0)
            break;
        uint32_t pos = start + best * win;
        int ok = 1;
        for (int i = 0; i < n; i++) {
            int32_t d = (int32_t)pos - (int32_t)out[i];
            if (d < 0)
                d = -d;
            if (d < minsep * (int32_t)win) {
                ok = 0;
                break;
            }
        }
        if (ok)
            out[n++] = pos;
        score[best] = 0;
    }
    free(e);
    free(score);
    /* sort */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j] < out[j - 1]; j--) {
            uint32_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

uint32_t sample_zero_cross(const Sample *s, uint32_t pos, uint32_t range)
{
    if (!s->data || s->frames < 2)
        return pos;
    if (pos >= s->frames)
        pos = s->frames - 1;
    for (uint32_t d = 0; d <= range; d++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int64_t i = (int64_t)pos + sign * (int64_t)d;
            if (i < 1 || i >= s->frames)
                continue;
            int a = mono_at(s, (uint32_t)i - 1), b = mono_at(s, (uint32_t)i);
            if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0))
                return (uint32_t)i;
        }
    }
    return pos;
}

int sample_save_wav(const char *path, const int16_t *data, uint32_t frames, int channels, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t bytes = frames * channels * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + bytes;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t w = 1;
    memcpy(h + 20, &w, 2);
    w = (uint16_t)channels;
    memcpy(h + 22, &w, 2);
    v = (uint32_t)rate;
    memcpy(h + 24, &v, 4);
    v = (uint32_t)rate * channels * 2;
    memcpy(h + 28, &v, 4);
    w = (uint16_t)(channels * 2);
    memcpy(h + 32, &w, 2);
    w = 16;
    memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4); /* little-endian hosts: x86 and ARM */
    int ok = fwrite(h, 1, 44, f) == 44 && fwrite(data, 2, (size_t)frames * channels, f) == (size_t)frames * channels;
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}

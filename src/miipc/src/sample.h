#ifndef SAMPLE_H
#define SAMPLE_H
#include <stdint.h>

/* Audio clip in memory: always 16-bit, mono or stereo. */
typedef struct {
    int16_t *data; /* interleaved */
    uint32_t frames;
    int channels; /* 1 eller 2 */
    int rate;
    char name[32];  /* file name without extension */
    char path[512]; /* full path, used when saving */
    /* Waveform cache (min/max per column over the whole clip). */
    int16_t *peaks;
    int peaks_w;
} Sample;

int sample_load_wav(Sample *s, const char *path);
void sample_free(Sample *s);

/* Min/max pairs for 'w' columns over the whole clip (cached). */
const int16_t *sample_peaks(Sample *s, int w);

/* Finds onsets (transients) between start and end. out[0] is always start.
 * Returns the number found (max 'max'), sorted ascending. */
int sample_detect_onsets(const Sample *s, uint32_t start, uint32_t end,
                         uint32_t *out, int max);

/* Nearest zero crossing within +-range frames (pos if none). */
uint32_t sample_zero_cross(const Sample *s, uint32_t pos, uint32_t range);
/* Writes 16-bit PCM WAV. */
int sample_save_wav(const char *path, const int16_t *data, uint32_t frames, int channels, int rate);

#endif

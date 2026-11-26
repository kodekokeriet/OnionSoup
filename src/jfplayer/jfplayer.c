// jfplayer: video player for the OnionSoup Jellyfin app.
//
// Plays a transcoded Jellyfin stream (MPEG-TS, H.264 + MP3/AAC) with
// FFmpeg 2.4 (the libraries Onion already ships) and draws its own
// on-screen controls: title, progress bar, time, pause, seeking, and menus
// for subtitles and audio tracks.
//
// Jellyfin streams are not seekable, so seeking, switching audio track and
// turning on subtitles all reopen the stream with StartTimeTicks,
// AudioStreamIndex and SubtitleStreamIndex (subtitles are burned in by the
// server). Playback progress is reported to Jellyfin so "continue watching"
// works.
//
// Usage: jfplayer <play file>
// The play file has key=value lines written by the Jellyfin app:
//   url, auth, server, item, title, duration, start, audio (index|name),
//   sub (index|name), audio_sel, sub_sel
//
// Buttons: A pause, Left/Right -/+10 s, L1/R1 -/+1 min, X subtitles,
// Y audio track, Up/Down or START show controls, B/MENU stop.
//
// Test environment: SDCARD, JF_FONT, JF_CURL, JFP_SCRIPT (scripted input),
// JFP_NO_AUDIO (no audio device), JFP_DEBUG (log to stderr), JFP_FLIP (turn
// the picture 180 degrees in software).

#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "soup/soup_ui.h"
#include "system/keymap_sw.h"

#define MAX_TRACKS 32
#define VQ_SIZE 3
#define AUDIO_RING (512 * 1024)
#define OSD_MS 3500
#define SEEK_DELAY_MS 700
#define REPORT_MS 10000

typedef struct {
    int index;
    char name[96];
} Track;

// what to play
static char base_url[1200], auth[400], server[300], item_id[40], title[200];
static char session_id[40], curl_bin[300] = "curl";
static double duration = 0;
static Track audios[MAX_TRACKS], subs[MAX_TRACKS];
static int n_audio = 0, n_sub = 0;
static int audio_sel = -1, sub_sel = -1;

// playback state shared with the decoder thread
typedef struct {
    SDL_Surface *surf;
    double pts;
} VFrame;

static SDL_mutex *mtx;
static SDL_cond *cond;
static VFrame vq[VQ_SIZE];
static int vq_r = 0, vq_n = 0;
static int frame_w = 0, frame_h = 0;

static Uint8 aring[AUDIO_RING];
static int ar_r = 0, ar_n = 0;
static double audio_end_pts = 0; // pts at the end of the data in the ring
static int out_rate = 44100;
static bool have_audio_dev = false;
static bool stream_has_audio = false;

static SDL_Thread *dec_thread = NULL;
static volatile bool abort_dec = false;
static volatile bool dec_eof = false;
static volatile bool dec_failed = false;
static volatile bool opened = false;
static char dec_error[200] = "";

static double open_start = 0; // position (s) the current stream was opened at
static volatile double first_pts = -1;
static bool paused = false;
static Uint32 wall_base = 0; // for streams without audio
static double wall_pos = 0;

// UI state
static bool quit = false;
static Uint32 osd_until = 0;
static double seek_target = -1;
static Uint32 seek_at = 0;
static int menu = 0; // 0 none, 1 subtitles, 2 audio
static int menu_sel = 0;
static double last_pos = 0;
static Uint32 last_report = 0;
static bool need_draw = true;
static bool flip_180 = false;
static SDL_Surface *cur_frame = NULL;

/* ---------- helpers ---------- */

static bool debug = false;
#define DBG(...)                                       \
    do {                                               \
        if (debug)                                     \
            fprintf(stderr, "jfplayer: " __VA_ARGS__); \
    } while (0)

static void fmt_time(double s, char *out, size_t n)
{
    if (s < 0)
        s = 0;
    int t = (int)s;
    if (t >= 3600)
        snprintf(out, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

static void new_session_id(void)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        session_id[i] = hex[rand() % 16];
    session_id[32] = '\0';
}

static bool load_play_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1600];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "url"))
            copy_str(base_url, sizeof(base_url), v);
        else if (!strcmp(k, "auth"))
            copy_str(auth, sizeof(auth), v);
        else if (!strcmp(k, "server"))
            copy_str(server, sizeof(server), v);
        else if (!strcmp(k, "item"))
            copy_str(item_id, sizeof(item_id), v);
        else if (!strcmp(k, "title"))
            copy_str(title, sizeof(title), v);
        else if (!strcmp(k, "duration"))
            duration = atof(v);
        else if (!strcmp(k, "start"))
            open_start = atof(v);
        else if (!strcmp(k, "audio_sel"))
            audio_sel = atoi(v);
        else if (!strcmp(k, "sub_sel"))
            sub_sel = atoi(v);
        else if ((!strcmp(k, "audio") && n_audio < MAX_TRACKS) || (!strcmp(k, "sub") && n_sub < MAX_TRACKS)) {
            Track *t = !strcmp(k, "audio") ? &audios[n_audio++] : &subs[n_sub++];
            t->index = atoi(v);
            const char *bar = strchr(v, '|');
            copy_str(t->name, sizeof(t->name), bar ? bar + 1 : v);
        }
    }
    fclose(f);
    return *base_url != '\0';
}

/* ---------- reporting to Jellyfin ---------- */

// POST JSON to the server in the background (or wait when sync is set)
static void report(const char *path, double pos, bool sync)
{
    if (!*server || !*item_id)
        return;
    char url[600], body[400], hdr[460];
    snprintf(url, sizeof(url), "%s%s", server, path);
    snprintf(hdr, sizeof(hdr), "Authorization: %s", auth);
    snprintf(body, sizeof(body),
             "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\",\"PositionTicks\":%lld,\"IsPaused\":%s,"
             "\"PlayMethod\":\"Transcode\",\"CanSeek\":true}",
             item_id, session_id, (long long)(pos * 10000000.0), paused ? "true" : "false");
    pid_t pid = fork();
    if (pid == 0) {
        if (!sync && fork() != 0)
            _exit(0); // double fork so nobody has to wait for curl
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp(curl_bin, curl_bin, "-s", "-k", "-m", "5", "-X", "POST", "-H", hdr, "-H",
               "Content-Type: application/json", "--data-binary", body, url, (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static void stop_encoding(void)
{
    if (!*server)
        return;
    char path[300], dev[80] = "";
    const char *d = strstr(base_url, "DeviceId=");
    if (d) {
        size_t l = strcspn(d + 9, "&");
        if (l < sizeof(dev))
            snprintf(dev, sizeof(dev), "%.*s", (int)l, d + 9);
    }
    snprintf(path, sizeof(path), "/Videos/ActiveEncodings?DeviceId=%s&PlaySessionId=%s", dev, session_id);
    char url[600], hdr[460];
    snprintf(url, sizeof(url), "%s%s", server, path);
    snprintf(hdr, sizeof(hdr), "Authorization: %s", auth);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp(curl_bin, curl_bin, "-s", "-k", "-m", "3", "-X", "DELETE", "-H", hdr, url, (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

/* ---------- clock ---------- */

static double bytes_per_sec(void) { return out_rate * 4.0; }

// Current media position in seconds (stream time, before open_start offset)
static double clock_pts(void)
{
    if (have_audio_dev && stream_has_audio) {
        SDL_LockMutex(mtx);
        double c = audio_end_pts - ar_n / bytes_per_sec();
        SDL_UnlockMutex(mtx);
        return c;
    }
    if (paused || wall_base == 0)
        return wall_pos;
    return wall_pos + (SDL_GetTicks() - wall_base) / 1000.0;
}

static double position(void)
{
    if (first_pts < 0)
        return open_start;
    double p = open_start + clock_pts() - first_pts;
    if (p < open_start)
        p = open_start;
    return p;
}

/* ---------- audio ---------- */

static void audio_cb(void *ud, Uint8 *out, int len)
{
    (void)ud;
    SDL_LockMutex(mtx);
    int n = paused ? 0 : (len < ar_n ? len : ar_n);
    int first = AUDIO_RING - ar_r < n ? AUDIO_RING - ar_r : n;
    memcpy(out, aring + ar_r, first);
    memcpy(out + first, aring, n - first);
    ar_r = (ar_r + n) % AUDIO_RING;
    ar_n -= n;
    SDL_CondBroadcast(cond);
    SDL_UnlockMutex(mtx);
    if (n < len)
        memset(out + n, 0, len - n);
}

// Append PCM to the ring, waiting while it is full. Returns false on abort.
static bool ring_push(const Uint8 *data, int len, double end_pts)
{
    SDL_LockMutex(mtx);
    while (!abort_dec && AUDIO_RING - ar_n < len)
        SDL_CondWaitTimeout(cond, mtx, 50);
    if (abort_dec) {
        SDL_UnlockMutex(mtx);
        return false;
    }
    int w = (ar_r + ar_n) % AUDIO_RING;
    int first = AUDIO_RING - w < len ? AUDIO_RING - w : len;
    memcpy(aring + w, data, first);
    memcpy(aring, data + first, len - first);
    ar_n += len;
    audio_end_pts = end_pts;
    SDL_UnlockMutex(mtx);
    return true;
}

/* ---------- decoder thread ---------- */

static int interrupt_cb(void *ud)
{
    (void)ud;
    return abort_dec;
}

static void build_url(char *out, size_t n, double start)
{
    char extra[200] = "";
    int l = 0;
    if (audio_sel >= 0)
        l += snprintf(extra + l, sizeof(extra) - l, "&AudioStreamIndex=%d", audio_sel);
    if (sub_sel >= 0)
        l += snprintf(extra + l, sizeof(extra) - l, "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", sub_sel);
    snprintf(out, n, "%s&StartTimeTicks=%lld%s&PlaySessionId=%s", base_url, (long long)(start * 10000000.0), extra,
             session_id);
}

// Wait for room in the video queue and return the slot to write, or NULL on abort
static VFrame *vq_slot(void)
{
    SDL_LockMutex(mtx);
    while (!abort_dec && vq_n >= VQ_SIZE)
        SDL_CondWaitTimeout(cond, mtx, 50);
    VFrame *f = abort_dec ? NULL : &vq[(vq_r + vq_n) % VQ_SIZE];
    SDL_UnlockMutex(mtx);
    return f;
}

static int decoder_main(void *ud)
{
    (void)ud;
    char url[1600];
    build_url(url, sizeof(url), open_start);

    AVFormatContext *fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = interrupt_cb;
    AVDictionary *opts = NULL;
    char hdr[460];
    snprintf(hdr, sizeof(hdr), "Authorization: %s\r\n", auth);
    if (*auth)
        av_dict_set(&opts, "headers", hdr, 0);
    av_dict_set(&opts, "timeout", "20000000", 0);
    av_dict_set(&opts, "analyzeduration", "2000000", 0);
    av_dict_set(&opts, "probesize", "1000000", 0);
    DBG("open at %.1f\n", open_start);
    if (avformat_open_input(&fmt, url, NULL, &opts) < 0) {
        DBG("open failed\n");
        av_dict_free(&opts);
        copy_str(dec_error, sizeof(dec_error), "Kunne ikke åpne videoen fra serveren.");
        dec_failed = true;
        return 0;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(fmt, NULL) < 0 && !abort_dec) {
        copy_str(dec_error, sizeof(dec_error), "Fant ikke video i strømmen.");
        dec_failed = true;
        avformat_close_input(&fmt);
        return 0;
    }

    AVCodec *vcodec = NULL, *acodec = NULL;
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vi, &acodec, 0);
    AVCodecContext *vctx = NULL, *actx = NULL;
    if (vi >= 0) {
        vctx = fmt->streams[vi]->codec;
        vctx->thread_count = 2;
        if (avcodec_open2(vctx, vcodec, NULL) < 0)
            vctx = NULL;
    }
    if (ai >= 0) {
        actx = fmt->streams[ai]->codec;
        if (avcodec_open2(actx, acodec, NULL) < 0)
            actx = NULL;
    }
    if (!vctx && !actx) {
        copy_str(dec_error, sizeof(dec_error), "Videoformatet støttes ikke.");
        dec_failed = true;
        avformat_close_input(&fmt);
        return 0;
    }
    stream_has_audio = actx != NULL;

    // video output size: fit 640x480, keep aspect
    int dw = SCREEN_W, dh = SCREEN_H;
    if (vctx && vctx->width > 0 && vctx->height > 0) {
        double sar = 1.0;
        AVRational r = av_guess_sample_aspect_ratio(fmt, fmt->streams[vi], NULL);
        if (r.num > 0 && r.den > 0)
            sar = av_q2d(r);
        double ar = vctx->width * sar / vctx->height;
        if (ar > (double)SCREEN_W / SCREEN_H)
            dh = (int)(SCREEN_W / ar) & ~1;
        else
            dw = (int)(SCREEN_H * ar) & ~1;
    }
    SDL_LockMutex(mtx);
    if (frame_w != dw || frame_h != dh) {
        for (int i = 0; i < VQ_SIZE; i++) {
            if (vq[i].surf)
                SDL_FreeSurface(vq[i].surf);
            vq[i].surf = SDL_CreateRGBSurface(SDL_SWSURFACE, dw, dh, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
        }
        frame_w = dw;
        frame_h = dh;
    }
    SDL_UnlockMutex(mtx);

    SwrContext *swr = NULL;
    struct SwsContext *sws = NULL;
    AVFrame *frame = av_frame_alloc();
    Uint8 *pcm = NULL;
    int pcm_cap = 0;
    opened = true;
    DBG("opened: video %d audio %d, %dx%d\n", vi, ai, dw, dh);

    AVPacket pkt;
    while (!abort_dec) {
        if (av_read_frame(fmt, &pkt) < 0)
            break;
        AVPacket p = pkt;
        if (vctx && pkt.stream_index == vi) {
            while (p.size > 0 && !abort_dec) {
                int got = 0;
                int used = avcodec_decode_video2(vctx, frame, &got, &p);
                if (used < 0)
                    break;
                p.size = 0; // video decoders consume the whole packet
                if (!got)
                    break;
                int64_t ts = av_frame_get_best_effort_timestamp(frame);
                double pts = ts == AV_NOPTS_VALUE ? 0 : ts * av_q2d(fmt->streams[vi]->time_base);
                if (first_pts < 0)
                    first_pts = pts;
                sws = sws_getCachedContext(sws, frame->width, frame->height, frame->format, dw, dh, AV_PIX_FMT_BGRA,
                                           SWS_FAST_BILINEAR, NULL, NULL, NULL);
                VFrame *slot = vq_slot();
                if (!slot || !sws)
                    break;
                SDL_LockSurface(slot->surf);
                uint8_t *dst[4] = {(uint8_t *)slot->surf->pixels, NULL, NULL, NULL};
                int dst_ls[4] = {slot->surf->pitch, 0, 0, 0};
                sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst, dst_ls);
                SDL_UnlockSurface(slot->surf);
                slot->pts = pts;
                SDL_LockMutex(mtx);
                vq_n++;
                SDL_UnlockMutex(mtx);
            }
        }
        else if (actx && pkt.stream_index == ai) {
            while (p.size > 0 && !abort_dec) {
                int got = 0;
                int used = avcodec_decode_audio4(actx, frame, &got, &p);
                if (used < 0)
                    break;
                p.data += used;
                p.size -= used;
                if (!got)
                    continue;
                int64_t ts = av_frame_get_best_effort_timestamp(frame);
                double pts = ts == AV_NOPTS_VALUE ? audio_end_pts : ts * av_q2d(fmt->streams[ai]->time_base);
                if (first_pts < 0)
                    first_pts = pts;
                if (!have_audio_dev)
                    continue;
                if (!swr) {
                    int64_t layout = frame->channel_layout ? frame->channel_layout
                                                           : av_get_default_channel_layout(frame->channels);
                    swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, out_rate, layout,
                                             frame->format, frame->sample_rate, 0, NULL);
                    if (!swr || swr_init(swr) < 0) {
                        swr_free(&swr);
                        have_audio_dev = false;
                        continue;
                    }
                }
                int max_out = (int)av_rescale_rnd(frame->nb_samples, out_rate, frame->sample_rate, AV_ROUND_UP) + 256;
                if (max_out * 4 > pcm_cap) {
                    pcm_cap = max_out * 4;
                    pcm = realloc(pcm, pcm_cap);
                }
                uint8_t *outp[1] = {pcm};
                int ns = swr_convert(swr, outp, max_out, (const uint8_t **)frame->extended_data, frame->nb_samples);
                if (ns > 0) {
                    double end = pts + (double)frame->nb_samples / frame->sample_rate;
                    if (!ring_push(pcm, ns * 4, end))
                        break;
                }
            }
        }
        av_free_packet(&pkt);
    }
    if (!abort_dec)
        dec_eof = true;
    DBG("decoder done (eof %d)\n", dec_eof);

    free(pcm);
    swr_free(&swr);
    sws_freeContext(sws);
    av_frame_free(&frame);
    if (vctx)
        avcodec_close(vctx);
    if (actx)
        avcodec_close(actx);
    avformat_close_input(&fmt);
    return 0;
}

static void stop_decoder(void)
{
    if (!dec_thread)
        return;
    abort_dec = true;
    SDL_LockMutex(mtx);
    SDL_CondBroadcast(cond);
    SDL_UnlockMutex(mtx);
    SDL_WaitThread(dec_thread, NULL);
    dec_thread = NULL;
}

// (Re)open the stream at the given position
static void start_at(double pos)
{
    stop_decoder();
    if (pos < 0)
        pos = 0;
    if (duration > 0 && pos > duration - 5)
        pos = duration - 5 > 0 ? duration - 5 : 0;
    stop_encoding();
    new_session_id();
    SDL_LockMutex(mtx);
    vq_r = vq_n = 0;
    ar_r = ar_n = 0;
    audio_end_pts = 0;
    SDL_UnlockMutex(mtx);
    open_start = pos;
    first_pts = -1;
    abort_dec = false;
    dec_eof = dec_failed = opened = false;
    wall_base = 0;
    wall_pos = 0;
    cur_frame = NULL;
    last_pos = pos;
    dec_thread = SDL_CreateThread(decoder_main, NULL);
    report("/Sessions/Playing", pos, false);
    last_report = SDL_GetTicks();
}

/* ---------- drawing ---------- */

static void icon_pause(int x, int y, int s, SDL_Color c)
{
    fill(x, y, s / 3, s, c);
    fill(x + s - s / 3, y, s / 3, s, c);
}

static void icon_play(int x, int y, int s, SDL_Color c)
{
    for (int i = 0; i < s; i++) {
        int w = i < s / 2 ? i : s - 1 - i;
        fill(x, y + i, w * 2 + 1, 1, c);
    }
}

static void draw_osd(double pos, bool buffering)
{
    // top bar with the title
    fill(0, 0, SCREEN_W, 44, C_BAR);
    text_fit(f19, title, 16, 10, SCREEN_W - 32, C_WHITE);

    // bottom panel: play state, times, progress bar, hints
    int y = SCREEN_H - 96;
    fill(0, y, SCREEN_W, 96, C_BAR);
    double shown = seek_target >= 0 ? seek_target : pos;
    char a[32], b[32];
    fmt_time(shown, a, sizeof(a));
    if (duration > 0)
        fmt_time(duration, b, sizeof(b));
    else
        copy_str(b, sizeof(b), "--:--");
    if (paused)
        icon_play(18, y + 14, 18, C_ACCENT);
    else
        icon_pause(18, y + 14, 18, C_ACCENT);
    text(f17, a, 50, y + 12, seek_target >= 0 ? C_ACCENT : C_WHITE);
    text(f17, b, SCREEN_W - 16 - text_w(f17, b), y + 12, C_GREY);
    if (buffering)
        text(f15, "Laster …", (SCREEN_W - text_w(f15, "Laster …")) / 2, y + 13, C_GREY);
    else if (sub_sel >= 0 || audio_sel >= 0) {
        char info[160] = "";
        for (int i = 0; i < n_sub; i++)
            if (subs[i].index == sub_sel)
                snprintf(info, sizeof(info), "Tekst: %s", subs[i].name);
        int iw = text_w(f13, info);
        if (iw > 300)
            iw = 300;
        text_fit(f13, info, (SCREEN_W - iw) / 2, y + 16, 300, C_GREY);
    }

    int bx = 16, bw = SCREEN_W - 32, by = y + 42;
    fill_round(bx, by, bw, 6, 3, C_CARD);
    if (duration > 0) {
        double f = shown / duration;
        if (f < 0)
            f = 0;
        if (f > 1)
            f = 1;
        int fw = (int)(bw * f);
        if (fw > 0)
            fill_round(bx, by, fw < 6 ? 6 : fw, 6, 3, C_ACCENT);
        fill_round(bx + fw - 7, by - 4, 14, 14, 7, C_WHITE);
    }

    const char *keys[] = {"A", "←→", "L/R", "X", "Y", "B"};
    const char *labels[] = {paused ? "Spill" : "Pause", "10 s", "1 min", "Tekst", "Lyd", "Stopp"};
    int x = 16;
    for (int i = 0; i < 6; i++) {
        int kw = text_w(f13, keys[i]);
        int kb = kw + 10 < 20 ? 20 : kw + 10;
        fill_round(x, y + 64, kb, 20, 10, C_ACCENT);
        text(f13, keys[i], x + (kb - kw) / 2, y + 65, C_DARK);
        text(f13, labels[i], x + kb + 5, y + 65, C_WHITE);
        x += kb + 5 + text_w(f13, labels[i]) + 16;
    }
}

static void draw_menu(void)
{
    bool is_sub = menu == 1;
    int n = is_sub ? n_sub + 1 : n_audio;
    int rows = n > 8 ? 8 : n;
    int w = 380, h = 64 + rows * 34, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 - 20;
    fill_round(x, y, w, h, 14, C_BG);
    text(f19, is_sub ? "Undertekster" : "Lydspor", x + 20, y + 14, C_WHITE);
    int top = menu_sel - rows + 1 > 0 ? menu_sel - rows + 1 : 0;
    for (int r = 0; r < rows; r++) {
        int i = top + r;
        if (i >= n)
            break;
        const char *name;
        int idx;
        if (is_sub) {
            name = i == 0 ? "Av" : subs[i - 1].name;
            idx = i == 0 ? -1 : subs[i - 1].index;
        }
        else {
            name = audios[i].name;
            idx = audios[i].index;
        }
        int ry = y + 52 + r * 34;
        if (i == menu_sel)
            fill_round(x + 10, ry - 4, w - 20, 32, 8, C_CARD);
        bool active = is_sub ? idx == sub_sel : (idx == audio_sel || (audio_sel < 0 && i == 0));
        if (active)
            fill_round(x + 22, ry + 8, 8, 8, 4, C_ACCENT);
        text_fit(f17, name, x + 40, ry, w - 60, i == menu_sel ? C_ACCENT : C_WHITE);
    }
}

static void draw(double pos)
{
    bool buffering =
        !dec_failed && (!opened || (vq_n == 0 && !dec_eof && (!cur_frame || (stream_has_audio && ar_n == 0))));
    SDL_FillRect(screen, NULL, 0);
    if (cur_frame) {
        SDL_Rect r = {(SCREEN_W - cur_frame->w) / 2, (SCREEN_H - cur_frame->h) / 2, 0, 0};
        SDL_BlitSurface(cur_frame, NULL, screen, &r);
    }
    if (dec_failed) {
        text(f24, "Avspillingen stoppet", (SCREEN_W - text_w(f24, "Avspillingen stoppet")) / 2, 190, C_WHITE);
        text(f15, dec_error, (SCREEN_W - text_w(f15, dec_error)) / 2, 230, C_GREY);
        text(f15, "Trykk A for å prøve igjen, B for å gå tilbake.",
             (SCREEN_W - text_w(f15, "Trykk A for å prøve igjen, B for å gå tilbake.")) / 2, 260, C_GREY);
    }
    else if (!cur_frame) {
        text(f24, "Laster …", (SCREEN_W - text_w(f24, "Laster …")) / 2, 200, C_WHITE);
        text_fit(f15, title, (SCREEN_W - text_w(f15, title)) / 2 > 20 ? (SCREEN_W - text_w(f15, title)) / 2 : 20,
                 240, SCREEN_W - 40, C_GREY);
    }
    bool show_osd = paused || menu || seek_target >= 0 || SDL_GetTicks() < osd_until || buffering;
    if (show_osd && !dec_failed)
        draw_osd(pos, buffering && cur_frame);
    if (menu)
        draw_menu();
    if (flip_180) {
        // fallback: turn the picture 180 degrees in software
        SDL_LockSurface(screen);
        Uint32 *px = (Uint32 *)screen->pixels;
        int n = screen->pitch / 4 * screen->h;
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            Uint32 t = px[i];
            px[i] = px[j];
            px[j] = t;
        }
        SDL_UnlockSurface(screen);
    }
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

/* ---------- input ---------- */

static void show_osd(void)
{
    osd_until = SDL_GetTicks() + OSD_MS;
    need_draw = true;
}

static void set_paused(bool p)
{
    if (p == paused)
        return;
    if (!stream_has_audio || !have_audio_dev) {
        if (p)
            wall_pos = clock_pts();
        else
            wall_base = SDL_GetTicks();
    }
    paused = p;
    report("/Sessions/Playing/Progress", position(), false);
}

static void seek_by(double d)
{
    if (seek_target < 0)
        seek_target = position();
    seek_target += d;
    if (seek_target < 0)
        seek_target = 0;
    if (duration > 0 && seek_target > duration - 5)
        seek_target = duration - 5;
    seek_at = SDL_GetTicks() + SEEK_DELAY_MS;
    show_osd();
}

static void on_key(SDLKey k)
{
    show_osd();
    if (menu) {
        int n = menu == 1 ? n_sub + 1 : n_audio;
        if (k == SW_BTN_UP && menu_sel > 0)
            menu_sel--;
        else if (k == SW_BTN_DOWN && menu_sel < n - 1)
            menu_sel++;
        else if (k == SW_BTN_B || k == SW_BTN_MENU || k == SW_BTN_X || k == SW_BTN_Y)
            menu = 0;
        else if (k == SW_BTN_A) {
            int idx = menu == 1 ? (menu_sel == 0 ? -1 : subs[menu_sel - 1].index) : audios[menu_sel].index;
            int *target = menu == 1 ? &sub_sel : &audio_sel;
            menu = 0;
            if (idx != *target) {
                *target = idx;
                start_at(position());
                paused = false;
            }
        }
        return;
    }
    if (dec_failed) {
        if (k == SW_BTN_A)
            start_at(last_pos);
        else if (k == SW_BTN_B || k == SW_BTN_MENU)
            quit = true;
        return;
    }
    switch (k) {
    case SW_BTN_A:
    case SW_BTN_START:
        if (k == SW_BTN_A)
            set_paused(!paused);
        break;
    case SW_BTN_LEFT:
        seek_by(-10);
        break;
    case SW_BTN_RIGHT:
        seek_by(10);
        break;
    case SW_BTN_L1:
        seek_by(-60);
        break;
    case SW_BTN_R1:
        seek_by(60);
        break;
    case SW_BTN_X:
        menu = 1;
        menu_sel = 0;
        for (int i = 0; i < n_sub; i++)
            if (subs[i].index == sub_sel)
                menu_sel = i + 1;
        break;
    case SW_BTN_Y:
        if (n_audio > 0) {
            menu = 2;
            menu_sel = 0;
            for (int i = 0; i < n_audio; i++)
                if (audios[i].index == audio_sel)
                    menu_sel = i;
        }
        break;
    case SW_BTN_B:
    case SW_BTN_MENU:
        if (seek_target >= 0)
            seek_target = -1;
        else
            quit = true;
        break;
    default:
        break;
    }
}

/* ---------- scripted input for tests ---------- */

static char *spos = NULL;
static Uint32 swait = 0;

static void script_step(void)
{
    if (!spos || SDL_GetTicks() < swait)
        return;
    while (*spos == ' ')
        spos++;
    if (!*spos) {
        quit = true;
        spos = NULL;
        return;
    }
    char tok[256];
    int n = 0;
    while (*spos && *spos != ' ' && n < 255)
        tok[n++] = *spos++;
    tok[n] = '\0';
    static const struct {
        const char *name;
        SDLKey key;
    } keys[] = {{"U", SW_BTN_UP}, {"D", SW_BTN_DOWN}, {"L", SW_BTN_LEFT}, {"R", SW_BTN_RIGHT}, {"A", SW_BTN_A}, {"B", SW_BTN_B}, {"X", SW_BTN_X}, {"Y", SW_BTN_Y}, {"L1", SW_BTN_L1}, {"R1", SW_BTN_R1}, {"START", SW_BTN_START}};
    if (!strncmp(tok, "shot:", 5)) {
        draw(position());
        SDL_SaveBMP(screen, tok + 5);
    }
    else if (!strncmp(tok, "wait:", 5))
        swait = SDL_GetTicks() + atoi(tok + 5);
    else if (!strcmp(tok, "pos"))
        fprintf(stderr, "jfplayer: position %.1f\n", position());
    else
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(tok, keys[i].name))
                on_key(keys[i].key);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc < 2 || !load_play_file(argv[1])) {
        fprintf(stderr, "usage: jfplayer <play file>\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    const char *e;
    char font_path[300];
    if ((e = getenv("JF_FONT")))
        copy_str(font_path, sizeof(font_path), e);
    else
        snprintf(font_path, sizeof(font_path), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf",
                 (e = getenv("SDCARD")) ? e : "/mnt/SDCARD");
    if ((e = getenv("JF_CURL")))
        copy_str(curl_bin, sizeof(curl_bin), e);
    debug = getenv("JFP_DEBUG") != NULL;
    if ((e = getenv("JFP_SCRIPT")))
        spos = strdup(e);

    av_register_all();
    avformat_network_init();
    av_log_set_level(AV_LOG_ERROR);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 120);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    // Same kind of surface as the other OnionSoup apps: on the Miyoo, the SDL
    // build turns the picture the right way up only when blitting from a
    // hardware surface to the screen.
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    flip_180 = getenv("JFP_FLIP") != NULL;
    if (!soup_open_fonts(font_path)) {
        fprintf(stderr, "jfplayer: cannot open font %s\n", font_path);
        return 1;
    }
    mtx = SDL_CreateMutex();
    cond = SDL_CreateCond();

    if (!getenv("JFP_NO_AUDIO")) {
        SDL_AudioSpec want = {0}, got;
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = audio_cb;
        if (SDL_OpenAudio(&want, &got) == 0 && got.format == AUDIO_S16SYS && got.channels == 2) {
            out_rate = got.freq;
            have_audio_dev = true;
            SDL_PauseAudio(0);
        }
    }

    start_at(open_start);
    show_osd();

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = true;
            else if (ev.type == SDL_KEYDOWN)
                on_key(ev.key.keysym.sym);
        }
        script_step();
        Uint32 now = SDL_GetTicks();

        if (seek_target >= 0 && now >= seek_at) {
            double t = seek_target;
            seek_target = -1;
            start_at(t);
            paused = false;
            need_draw = true;
        }

        // start the wall clock for streams without audio
        if (opened && first_pts >= 0 && wall_base == 0 && !paused) {
            wall_pos = first_pts;
            wall_base = now;
        }

        // pick the video frame to show
        double clk = clock_pts();
        SDL_LockMutex(mtx);
        while (vq_n > 0 && !paused) {
            VFrame *f = &vq[vq_r];
            bool late = vq_n > 1 && vq[(vq_r + 1) % VQ_SIZE].pts <= clk;
            if (f->pts <= clk + 0.01 || !cur_frame) {
                // copy out so the decoder can reuse the slot
                static SDL_Surface *shown = NULL;
                if (!shown || shown->w != f->surf->w || shown->h != f->surf->h) {
                    if (shown)
                        SDL_FreeSurface(shown);
                    shown = SDL_CreateRGBSurface(SDL_SWSURFACE, f->surf->w, f->surf->h, 32, 0x00FF0000, 0x0000FF00,
                                                 0x000000FF, 0);
                }
                SDL_BlitSurface(f->surf, NULL, shown, NULL);
                cur_frame = shown;
                vq_r = (vq_r + 1) % VQ_SIZE;
                vq_n--;
                SDL_CondBroadcast(cond);
                need_draw = true;
                if (!late)
                    break;
            }
            else
                break;
        }
        SDL_UnlockMutex(mtx);

        double pos = position();
        if ((int)pos != (int)last_pos)
            need_draw = true;
        if (opened)
            last_pos = pos;

        if (now - last_report > REPORT_MS && opened) {
            report("/Sessions/Playing/Progress", pos, false);
            last_report = now;
        }

        // end of the film
        if (dec_eof && vq_n == 0 && ar_n == 0 && !paused)
            quit = true;

        bool osd_visible = paused || menu || seek_target >= 0 || now < osd_until;
        static bool was_visible = false;
        if (osd_visible != was_visible) {
            need_draw = true;
            was_visible = osd_visible;
        }
        if (need_draw) {
            draw(pos);
            need_draw = false;
        }
        SDL_Delay(5);
    }

    double end_pos = last_pos;
    stop_decoder();
    SDL_CloseAudio();
    report("/Sessions/Playing/Stopped", end_pos, true);
    stop_encoding();
    FILE *f = fopen("/tmp/jellyfin_result", "w");
    if (f) {
        fprintf(f, "%.0f\n", end_pos);
        fclose(f);
    }
    TTF_Quit();
    SDL_Quit();
    return 0;
}

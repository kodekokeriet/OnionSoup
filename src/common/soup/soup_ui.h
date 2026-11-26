// Shared drawing helpers for OnionSoup apps (App Store, Jellyfin, ...).
// Include once per program; it defines the globals `video`, `screen` and fonts f13..f34.
#ifndef SOUP_UI_H__
#define SOUP_UI_H__

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Not every program uses every helper
#pragma GCC diagnostic ignored "-Wunused-function"

#define SCREEN_W 640
#define SCREEN_H 480
#define TOP_H 52
#define BOTTOM_Y 446

static SDL_Surface *video, *screen;
static TTF_Font *f13, *f15, *f17, *f19, *f24, *f34;

// OnionSoup palette
static const SDL_Color C_BG = {37, 32, 29};
static const SDL_Color C_BAR = {28, 24, 22};
static const SDL_Color C_PANEL = {48, 42, 37};
static const SDL_Color C_CARD = {58, 51, 45};
static const SDL_Color C_ACCENT = {255, 157, 90};
static const SDL_Color C_WHITE = {255, 255, 255};
static const SDL_Color C_SOFT = {225, 215, 205};
static const SDL_Color C_GREY = {160, 150, 140};
static const SDL_Color C_DARK = {30, 25, 22};
static const SDL_Color C_GREEN = {90, 200, 120};
static const SDL_Color C_RED = {240, 110, 100};

/* ---------- helpers ---------- */

static void copy_str(char *dst, size_t n, const char *src)
{
    if (!src)
        src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

static Uint32 map(SDL_Surface *s, SDL_Color c)
{
    return SDL_MapRGB(s->format, c.r, c.g, c.b);
}

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void read_file_line(const char *path, char *out, size_t n)
{
    out[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    if (fgets(out, n, fp)) {
        out[strcspn(out, "\r\n")] = '\0';
    }
    fclose(fp);
}

static char *read_all(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (buf) {
        size_t got = fread(buf, 1, len, fp);
        buf[got] = '\0';
    }
    fclose(fp);
    return buf;
}

static void human_size(long bytes, char *out, size_t n)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, n, "%.1f MB", bytes / 1048576.0);
    else
        snprintf(out, n, "%ld KB", (bytes + 1023) / 1024);
}

/* ---------- drawing primitives ---------- */

static void fill(int x, int y, int w, int h, SDL_Color c)
{
    SDL_Rect r = {x, y, w, h};
    SDL_FillRect(screen, &r, map(screen, c));
}

// Rounded rectangle, drawn as horizontal spans
static void fill_round(int x, int y, int w, int h, int r, SDL_Color c)
{
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    Uint32 col = map(screen, c);
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = -1;
        if (j < r)
            dy = r - j;
        else if (j >= h - r)
            dy = j - (h - r - 1);
        if (dy > 0) {
            float d = (float)dy - 0.5f;
            float dx = (float)r - sqrtf((float)(r * r) - d * d);
            inset = (int)(dx + 0.5f);
        }
        SDL_Rect span = {x + inset, y + j, w - inset * 2, 1};
        SDL_FillRect(screen, &span, col);
    }
}

static void outline_round(int x, int y, int w, int h, int r, int t, SDL_Color c, SDL_Color inner)
{
    fill_round(x, y, w, h, r, c);
    fill_round(x + t, y + t, w - t * 2, h - t * 2, r > t ? r - t : 0, inner);
}

static int text_w(TTF_Font *f, const char *s)
{
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s, &w, &h);
    return w;
}

static void text(TTF_Font *f, const char *s, int x, int y, SDL_Color c)
{
    if (!s || !*s)
        return;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, s, c);
    if (!t)
        return;
    SDL_Rect pos = {x, y};
    SDL_BlitSurface(t, NULL, screen, &pos);
    SDL_FreeSurface(t);
}

// Draw text cut to max_w pixels, adding an ellipsis
static void text_fit(TTF_Font *f, const char *s, int x, int y, int max_w, SDL_Color c)
{
    char buf[256];
    copy_str(buf, sizeof(buf), s);
    if (text_w(f, buf) <= max_w) {
        text(f, buf, x, y, c);
        return;
    }
    int len = strlen(buf);
    while (len > 0) {
        // step back over UTF-8 continuation bytes
        do {
            len--;
        } while (len > 0 && (buf[len] & 0xC0) == 0x80);
        buf[len] = '\0';
        char tmp[260];
        snprintf(tmp, sizeof(tmp), "%s…", buf);
        if (text_w(f, tmp) <= max_w) {
            text(f, tmp, x, y, c);
            return;
        }
    }
}

// Word-wrapped text, returns the y after the last line
static int text_wrap(TTF_Font *f, const char *s, int x, int y, int max_w, int line_h, int max_lines, SDL_Color c)
{
    char line[256] = "";
    char word[128];
    const char *p = s;
    int lines = 0;
    while (*p && lines < max_lines) {
        int n = 0;
        while (*p == ' ')
            p++;
        while (*p && *p != ' ' && n < (int)sizeof(word) - 1)
            word[n++] = *p++;
        word[n] = '\0';
        if (!n)
            break;
        char trial[400];
        snprintf(trial, sizeof(trial), "%s%s%s", line, *line ? " " : "", word);
        if (text_w(f, trial) > max_w && *line) {
            text(f, line, x, y, c);
            y += line_h;
            lines++;
            copy_str(line, sizeof(line), word);
        }
        else {
            copy_str(line, sizeof(line), trial);
        }
    }
    if (*line && lines < max_lines) {
        text(f, line, x, y, c);
        y += line_h;
    }
    return y;
}

/* ---------- images ---------- */

static Uint32 get_px(SDL_Surface *s, int x, int y)
{
    return ((Uint32 *)((Uint8 *)s->pixels + y * s->pitch))[x];
}

// Bilinear scale of a 32-bit surface into a new ARGB surface with rounded corners
static SDL_Surface *scale_rounded(SDL_Surface *src, int w, int h, int radius)
{
    SDL_Surface *s32 = SDL_CreateRGBSurface(SDL_SWSURFACE, src->w, src->h, 32,
                                            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    SDL_Surface *dst = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32,
                                            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!s32 || !dst)
        return NULL;
    SDL_SetAlpha(src, 0, 255);
    SDL_BlitSurface(src, NULL, s32, NULL);
    SDL_LockSurface(s32);
    SDL_LockSurface(dst);
    for (int y = 0; y < h; y++) {
        float sy = (y + 0.5f) * s32->h / h - 0.5f;
        int y0 = sy < 0 ? 0 : (int)sy;
        int y1 = y0 + 1 < s32->h ? y0 + 1 : y0;
        float fy = sy - y0;
        if (fy < 0)
            fy = 0;
        for (int x = 0; x < w; x++) {
            float sx = (x + 0.5f) * s32->w / w - 0.5f;
            int x0 = sx < 0 ? 0 : (int)sx;
            int x1 = x0 + 1 < s32->w ? x0 + 1 : x0;
            float fx = sx - x0;
            if (fx < 0)
                fx = 0;
            Uint32 p00 = get_px(s32, x0, y0), p10 = get_px(s32, x1, y0);
            Uint32 p01 = get_px(s32, x0, y1), p11 = get_px(s32, x1, y1);
            Uint32 out = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                float a = ((p00 >> sh) & 255) * (1 - fx) + ((p10 >> sh) & 255) * fx;
                float b = ((p01 >> sh) & 255) * (1 - fx) + ((p11 >> sh) & 255) * fx;
                out |= ((Uint32)(a * (1 - fy) + b * fy) & 255) << sh;
            }
            // rounded corner alpha (anti-aliased)
            float alpha = 1.0f;
            float cx = x < radius ? radius - x - 0.5f : (x >= w - radius ? x - (w - radius) + 0.5f : 0);
            float cy = y < radius ? radius - y - 0.5f : (y >= h - radius ? y - (h - radius) + 0.5f : 0);
            if (cx > 0 && cy > 0) {
                float d = sqrtf(cx * cx + cy * cy) - radius;
                alpha = d >= 0.5f ? 0 : (d <= -0.5f ? 1 : 0.5f - d);
            }
            out |= (Uint32)(alpha * 255) << 24;
            ((Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch))[x] = out;
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(s32);
    SDL_FreeSurface(s32);
    return dst;
}

static void draw_hints(const char *const *keys, const char *const *labels, int n)
{
    fill(0, BOTTOM_Y, SCREEN_W, SCREEN_H - BOTTOM_Y, C_BAR);
    int x = 16;
    for (int i = 0; i < n; i++) {
        int kw = text_w(f15, keys[i]);
        int bw = kw + 12 < 22 ? 22 : kw + 12;
        fill_round(x, 453, bw, 22, 11, C_ACCENT);
        text(f15, keys[i], x + (bw - kw) / 2, 454, C_DARK);
        text(f15, labels[i], x + bw + 6, 453, C_WHITE);
        x += bw + 6 + text_w(f15, labels[i]) + 20;
    }
}

// Open the fonts at the standard sizes. Returns false if the font is missing.
static bool soup_open_fonts(const char *path)
{
    f13 = TTF_OpenFont(path, 13);
    f15 = TTF_OpenFont(path, 15);
    f17 = TTF_OpenFont(path, 17);
    f19 = TTF_OpenFont(path, 19);
    f24 = TTF_OpenFont(path, 24);
    f34 = TTF_OpenFont(path, 34);
    return f13 && f15 && f17 && f19 && f24 && f34;
}

// "onionsoup <subtitle>" logo at the top left
static void soup_logo(const char *subtitle)
{
    fill(0, 0, SCREEN_W, TOP_H, C_BAR);
    text(f24, "onion", 18, 10, C_ACCENT);
    int x = 18 + text_w(f24, "onion");
    text(f24, "soup", x, 10, C_WHITE);
    x += text_w(f24, "soup") + 8;
    text(f17, subtitle, x, 16, C_GREY);
}

// Header with the app's own name, e.g. soup_title("Game", "Store")
static void soup_title(const char *first, const char *second)
{
    fill(0, 0, SCREEN_W, TOP_H, C_BAR);
    text(f24, first, 18, 10, C_ACCENT);
    text(f24, second, 18 + text_w(f24, first) + 6, 10, C_WHITE);
}

#endif // SOUP_UI_H__

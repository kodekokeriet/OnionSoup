#include "gfx.h"
#include "font.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int SCREEN_W = 640, SCREEN_H = 480;
static SDL_Surface *screen;

static void detect_resolution(void)
{
    const char *r = getenv("MIIPC_RES");
    if (r && sscanf(r, "%dx%d", &SCREEN_W, &SCREEN_H) == 2)
        return;
#ifdef MIYOO
    /* Mini v4 has a 752x560 panel; every other model is 640x480. */
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    int w = 0, h = 0;
    if (f) {
        if (fscanf(f, "%d,%d", &w, &h) != 2)
            w = 0;
        fclose(f);
    }
    if (w == 752) {
        SCREEN_W = 752;
        SCREEN_H = 560;
    }
#endif
}

#if SDL_MAJOR_VERSION >= 2
/* PC build: draw into a software surface, show it scaled in a resizable window. */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;

int gfx_init(void)
{
    detect_resolution();
    screen = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!screen)
        return -1;
    int scale = 2;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        while (scale > 1 && (SCREEN_W * scale > dm.w * 9 / 10 || SCREEN_H * scale > dm.h * 9 / 10))
            scale--;
    }
    win = SDL_CreateWindow("MiiPC  (F1 = help)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           SCREEN_W * scale, SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    if (!win)
        return -1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* crisp pixels */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (ren) {
        SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    }
    return 0;
}

void gfx_present(void)
{
    if (!ren || !tex)
        return;
    SDL_UpdateTexture(tex, NULL, screen->pixels, screen->pitch);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}
#else
/* Miyoo: the panel is mounted upside down. The Miyoo SDL only turns the picture
 * when one HW surface is blitted to another (that blit runs on the MI_GFX
 * hardware, which rotates it). So, like the other OnionSoup apps, we draw into a
 * HW surface of our own and blit that to the HW video surface. MIIPC_FLIP=1 turns it in software
 * instead, for an SDL that does not. */
static SDL_Surface *video;
static int soft_flip;

int gfx_init(void)
{
    detect_resolution();
    soft_flip = getenv("MIIPC_FLIP") != NULL;
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    if (!video && (SCREEN_W != 640 || SCREEN_H != 480)) {
        SCREEN_W = 640;
        SCREEN_H = 480;
        video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    }
    if (!video)
        video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_HWSURFACE);
    if (!video)
        return -1;
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!screen)
        return -1;
    SDL_ShowCursor(SDL_DISABLE);
    SDL_WM_SetCaption("MiiPC", NULL);
    return 0;
}

/* turn 180 degrees in place: swap pixel i with pixel n-1-i */
static void turn_180(void)
{
    SDL_LockSurface(screen);
    Uint32 *a = (Uint32 *)screen->pixels;
    int n = screen->pitch / 4 * SCREEN_H;
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        Uint32 t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    SDL_UnlockSurface(screen);
}

void gfx_present(void)
{
    if (soft_flip)
        turn_180();
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
    if (soft_flip)
        turn_180(); /* back again: the app may draw on top of the last frame */
}
#endif

void gfx_quit(void) {}

static Uint32 map(Color c)
{
    return SDL_MapRGB(screen->format, (c >> 16) & 255, (c >> 8) & 255, c & 255);
}

void gfx_clear(Color c) { SDL_FillRect(screen, NULL, map(c)); }

void gfx_rect(int x, int y, int w, int h, Color c)
{
    if (w <= 0 || h <= 0)
        return;
    SDL_Rect r = {(Sint16)x, (Sint16)y, (Uint16)w, (Uint16)h};
    SDL_FillRect(screen, &r, map(c));
}

void gfx_frame(int x, int y, int w, int h, int t, Color c)
{
    gfx_rect(x, y, w, t, c);
    gfx_rect(x, y + h - t, w, t, c);
    gfx_rect(x, y, t, h, c);
    gfx_rect(x + w - t, y, t, h, c);
}

void gfx_vline(int x, int y0, int y1, Color c)
{
    if (y1 < y0) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    gfx_rect(x, y0, 1, y1 - y0 + 1, c);
}

/* Next character as a glyph index into the font. */
static int next_glyph(const unsigned char **s)
{
    unsigned c = *(*s)++;
    if (c >= 32 && c < 127)
        return c - 32;
    if (c == 0xC3 && **s) {
        unsigned d = *(*s)++;
        switch (d) {
        case 0xA6:
            return 95; /* æ */
        case 0xB8:
            return 96; /* ø */
        case 0xA5:
            return 97; /* å */
        case 0x86:
            return 98; /* Æ */
        case 0x98:
            return 99; /* Ø */
        case 0x85:
            return 100; /* Å */
        }
    }
    else if (c >= 0xC0) {
        while ((**s & 0xC0) == 0x80)
            (*s)++;
    }
    return '?' - 32;
}

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        next_glyph(&p);
        n++;
    }
    return n * FONT_W * scale;
}

int gfx_text(int x, int y, int scale, Color c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    Uint32 px = map(c);
    int bpp = screen->format->BytesPerPixel, x0 = x;
    SDL_LockSurface(screen);
    const unsigned char *s = (const unsigned char *)buf;
    while (*s) {
        const unsigned char *g = font8x12[next_glyph(&s)];
        for (int gy = 0; gy < FONT_H; gy++)
            for (int gx = 0; gx < FONT_W; gx++) {
                if (!(g[gy] & (0x80 >> gx)))
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int X = x + gx * scale + sx, Y = y + gy * scale + sy;
                        if (X < 0 || Y < 0 || X >= SCREEN_W || Y >= SCREEN_H)
                            continue;
                        Uint8 *p = (Uint8 *)screen->pixels + Y * screen->pitch + X * bpp;
                        if (bpp == 4)
                            *(Uint32 *)p = px;
                        else
                            *(Uint16 *)p = (Uint16)px;
                    }
            }
        x += FONT_W * scale;
    }
    SDL_UnlockSurface(screen);
    return x - x0;
}

int gfx_save(const char *p) { return SDL_SaveBMP(screen, p); }

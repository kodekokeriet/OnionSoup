#ifndef GFX_H
#define GFX_H
#include <SDL.h>
#include <stdint.h>

/* Screen size is detected at startup:
 *   Miyoo Mini v1-v3 and Mini Plus: 640x480,  Mini v4: 752x560.
 *   On PC it can be overridden with MIIPC_RES=752x560. */
extern int SCREEN_W, SCREEN_H;

typedef uint32_t Color; /* 0xRRGGBB */

int  gfx_init(void);
void gfx_quit(void);
void gfx_clear(Color c);
void gfx_rect(int x, int y, int w, int h, Color c);
void gfx_frame(int x, int y, int w, int h, int t, Color c);
void gfx_vline(int x, int y0, int y1, Color c);
/* UTF-8 text (ASCII + a few Latin-1 letters). Returns width in pixels. */
int  gfx_text(int x, int y, int scale, Color c, const char *fmt, ...);
int  gfx_text_width(const char *s, int scale);
void gfx_present(void);
int  gfx_save(const char *bmp_path);

#endif

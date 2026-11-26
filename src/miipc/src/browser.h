/* File browser for the SD card: folders + files with a given extension. */
#ifndef BROWSER_H
#define BROWSER_H
#include "input.h"

typedef struct {
    int   active;
    char  dir[512];
    char  ext[8];          /* e.g. ".wav" */
    char  title[48];
    char **names;
    unsigned char *isdir;
    int   n, sel, scroll;
} Browser;

void browser_open(Browser *b, const char *dir, const char *ext, const char *title);
void browser_close(Browser *b);
/* Returns 1 = file chosen (path filled), -1 = cancelled, 0 = nothing. */
int  browser_key(Browser *b, SDLKey k, char *path, int len);
/* Full path of the highlighted file, or 0 if it is a folder. */
int  browser_current_file(const Browser *b, char *path, int len);
void browser_draw(const Browser *b, int x, int y, int w, int h);

#endif

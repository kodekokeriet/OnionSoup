#include "browser.h"
#include "gfx.h"
#include "help.h"
#include "input.h"
#include "theme.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void free_list(Browser *b)
{
    for (int i = 0; i < b->n; i++)
        free(b->names[i]);
    free(b->names);
    free(b->isdir);
    b->names = NULL;
    b->isdir = NULL;
    b->n = 0;
}

static Browser *sort_ctx;
static int cmp_idx(const void *a, const void *b)
{
    int i = *(const int *)a, j = *(const int *)b;
    if (sort_ctx->isdir[i] != sort_ctx->isdir[j])
        return sort_ctx->isdir[j] - sort_ctx->isdir[i];
    return strcasecmp(sort_ctx->names[i], sort_ctx->names[j]);
}

static void scan(Browser *b)
{
    free_list(b);
    DIR *d = opendir(b->dir);
    int cap = 64;
    char **names = malloc(sizeof(char *) * cap);
    unsigned char *isdir = malloc(cap);
    int n = 0;
    if (strcmp(b->dir, "/")) {
        names[n] = strdup("..");
        isdir[n++] = 1;
    }
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue; /* hidden files and . / .. */
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", b->dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        int dir = S_ISDIR(st.st_mode);
        if (!dir) {
            const char *e = strrchr(de->d_name, '.');
            if (!e || strcasecmp(e, b->ext))
                continue;
        }
        if (n == cap) {
            cap *= 2;
            names = realloc(names, sizeof(char *) * cap);
            isdir = realloc(isdir, cap);
        }
        names[n] = strdup(de->d_name);
        isdir[n++] = dir;
    }
    if (d)
        closedir(d);
    /* sort (folders first), ".." stays on top */
    int off = (n && !strcmp(names[0], "..")) ? 1 : 0;
    int *idx = malloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < n; i++)
        idx[i] = i;
    b->names = names;
    b->isdir = isdir;
    b->n = n;
    sort_ctx = b;
    qsort(idx + off, n - off, sizeof(int), cmp_idx);
    char **sn = malloc(sizeof(char *) * (n ? n : 1));
    unsigned char *sd = malloc(n ? n : 1);
    for (int i = 0; i < n; i++) {
        sn[i] = names[idx[i]];
        sd[i] = isdir[idx[i]];
    }
    free(names);
    free(isdir);
    free(idx);
    b->names = sn;
    b->isdir = sd;
    b->sel = 0;
    b->scroll = 0;
}

void browser_open(Browser *b, const char *dir, const char *ext, const char *title)
{
    char real[PATH_MAX];
    if (!realpath(dir, real)) {
        if (!getcwd(real, sizeof real))
            snprintf(real, sizeof real, "/");
    }
    snprintf(b->dir, sizeof b->dir, "%s", real);
    snprintf(b->ext, sizeof b->ext, "%s", ext);
    snprintf(b->title, sizeof b->title, "%s", title);
    b->active = 1;
    scan(b);
}

void browser_close(Browser *b)
{
    b->active = 0;
    free_list(b);
}

int browser_current_file(const Browser *b, char *path, int len)
{
    if (b->n == 0 || b->isdir[b->sel])
        return 0;
    snprintf(path, len, "%s/%s", strcmp(b->dir, "/") ? b->dir : "", b->names[b->sel]);
    return 1;
}

static void go_up(Browser *b)
{
    char *sl = strrchr(b->dir, '/');
    if (!sl)
        return;
    char prev[256];
    snprintf(prev, sizeof prev, "%s", sl + 1);
    if (sl == b->dir)
        b->dir[1] = 0;
    else
        *sl = 0;
    scan(b);
    for (int i = 0; i < b->n; i++)
        if (!strcmp(b->names[i], prev)) {
            b->sel = i;
            break;
        }
}

int browser_key(Browser *b, SDLKey k, char *path, int len)
{
    if (k == BTN_UP && b->n)
        b->sel = (b->sel + b->n - 1) % b->n;
    if (k == BTN_DOWN && b->n)
        b->sel = (b->sel + 1) % b->n;
    if (k == BTN_L1 || k == BTN_LEFT) {
        b->sel -= 8;
        if (b->sel < 0)
            b->sel = 0;
    }
    if (k == BTN_R1 || k == BTN_RIGHT) {
        b->sel += 8;
        if (b->sel >= b->n)
            b->sel = b->n ? b->n - 1 : 0;
    }
    if (k == BTN_B) {
        if (!strcmp(b->dir, "/"))
            return -1;
        go_up(b);
    }
    if (k == BTN_MENU || k == BTN_SELECT)
        return -1;
    if (k == BTN_A && b->n) {
        if (b->isdir[b->sel]) {
            if (!strcmp(b->names[b->sel], ".."))
                go_up(b);
            else {
                char nd[512];
                snprintf(nd, sizeof nd, "%s/%s", strcmp(b->dir, "/") ? b->dir : "", b->names[b->sel]);
                snprintf(b->dir, sizeof b->dir, "%s", nd);
                scan(b);
            }
        }
        else {
            browser_current_file(b, path, len);
            return 1;
        }
    }
    return 0;
}

void browser_draw(const Browser *b, int x, int y, int w, int h)
{
    gfx_rect(x, y, w, h, C_PANEL);
    gfx_frame(x, y, w, h, 2, C_ACCENT);
    gfx_text(x + 10, y + 8, 1, C_ACCENT, "%s", b->title);
    /* show the tail of long paths */
    int maxc = (w - 20) / 8;
    const char *d = b->dir;
    int dl = (int)strlen(d);
    gfx_text(x + 10, y + 24, 1, C_DIM, "%s%s", dl > maxc ? "..." : "", dl > maxc ? d + dl - maxc + 3 : d);

    int rowh = 18, top = y + 46, rows = (h - 46 - 26) / rowh;
    Browser *mb = (Browser *)b;
    if (mb->sel < mb->scroll)
        mb->scroll = mb->sel;
    if (mb->sel >= mb->scroll + rows)
        mb->scroll = mb->sel - rows + 1;
    if (b->n == 0)
        gfx_text(x + 10, top, 1, C_DIM, "(no %s files here)", b->ext);
    for (int r = 0; r < rows && b->scroll + r < b->n; r++) {
        int i = b->scroll + r, ry = top + r * rowh;
        if (i == b->sel)
            gfx_rect(x + 4, ry - 3, w - 8, rowh, C_SEL);
        char nm[128];
        snprintf(nm, sizeof nm, "%s%s", b->names[i], b->isdir[i] ? "/" : "");
        if ((int)strlen(nm) > maxc - 2)
            nm[maxc - 2] = 0;
        gfx_text(x + 12, ry, 1, b->isdir[i] ? C_STEP : C_TEXT, "%s", nm);
    }
    if (b->n > rows) { /* scrollbar */
        int bh = (h - 72) * rows / b->n, by = top + (h - 72 - bh) * b->scroll / (b->n - rows);
        gfx_rect(x + w - 8, by, 3, bh, C_DIM);
    }
    gfx_text(x + 10, y + h - 20, 1, C_DIM, "%s", K("A open/choose   B back   X listen   {SEL} cancel"));
}

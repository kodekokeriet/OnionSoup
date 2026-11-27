// Bokhylle: OnionSoup's ebook reader. A visual shelf of the books in Media/Books and a reader
// for EPUB and TXT, in the same look as the App Store and Jellyfin apps.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "soup/soup_ui.h"
#include "stb/stb_image.h"
#include "system/keymap_sw.h"

#include "book.h"

#define APP_VERSION "1.0.0"
#define CARD_W 100
#define CARD_H 140
#define DETAIL_W 180
#define DETAIL_H 253
#define MAX_TABS 12

/* ---------- library ---------- */

typedef struct {
    char file[512], rel[384], folder[96];
    char title[256], author[256];
    long size, mtime, added;
    int chapter, offset;
    double progress;
    long last_read;
    bool finished;
    bool meta_done;
    int cover_state; // 0 = not loaded, 1 = loaded
    SDL_Surface *card, *big;
} Entry;

typedef enum { S_SHELF,
               S_DETAIL,
               S_READ } ScreenId;
typedef enum { SORT_TITLE,
               SORT_AUTHOR,
               SORT_RECENT,
               SORT_ADDED,
               SORT_COUNT } SortMode;
static const char *sort_names[] = {"Title", "Author", "Last read", "Recently added"};

static char sdcard[256], books_dir[400], state_dir[400], cover_dir[460], lib_path[460], settings_path[460];
static char ui_font[400], font_dir[420];
static Entry *lib = NULL;
static int nlib = 0;
static ScreenId scr = S_SHELF;
static volatile sig_atomic_t quit = 0;
static bool dirty = true;
static bool lib_dirty = false;
static Uint32 lib_changed_at = 0;

// shelf views: tab -> filtered, sorted indices into lib
typedef struct {
    char name[96];
    int kind; // 0 = all, 1 = reading, 2 = finished, 3 = folder
} Tab;
static Tab tabs[MAX_TABS];
static int ntabs = 0, tab_sel = 0;
static int *view = NULL, nview = 0, view_sel = 0, view_scroll = 0;
static SortMode sort_mode = SORT_TITLE;
static Entry *detail = NULL;
static char *detail_desc = NULL;
static int detail_chapters = 0;

static char message[200] = "";
static Uint32 message_until = 0;

static void show_message(const char *t, Uint32 ms)
{
    copy_str(message, sizeof(message), t);
    message_until = SDL_GetTicks() + ms;
    dirty = true;
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    copy_str(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static unsigned long hash_str(const char *s, long extra)
{
    unsigned long h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    h = (h ^ (unsigned long)extra) * 16777619u;
    return h & 0xFFFFFFFFu;
}

static void mark_lib_dirty(void)
{
    lib_dirty = true;
    lib_changed_at = SDL_GetTicks();
}

static void save_library(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *books = cJSON_AddObjectToObject(root, "books");
    for (int i = 0; i < nlib; i++) {
        Entry *e = &lib[i];
        cJSON *o = cJSON_AddObjectToObject(books, e->rel);
        cJSON_AddStringToObject(o, "title", e->title);
        cJSON_AddStringToObject(o, "author", e->author);
        cJSON_AddNumberToObject(o, "size", e->size);
        cJSON_AddNumberToObject(o, "mtime", e->mtime);
        cJSON_AddNumberToObject(o, "added", e->added);
        cJSON_AddBoolToObject(o, "meta", e->meta_done);
        cJSON_AddNumberToObject(o, "chapter", e->chapter);
        cJSON_AddNumberToObject(o, "offset", e->offset);
        cJSON_AddNumberToObject(o, "progress", e->progress);
        cJSON_AddNumberToObject(o, "last", e->last_read);
        cJSON_AddBoolToObject(o, "finished", e->finished);
    }
    cJSON_AddNumberToObject(root, "sort", sort_mode);
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (!s)
        return;
    char tmp[480];
    snprintf(tmp, sizeof(tmp), "%s.tmp", lib_path);
    FILE *fp = fopen(tmp, "w");
    if (fp) {
        fputs(s, fp);
        fclose(fp);
        rename(tmp, lib_path);
    }
    free(s);
    lib_dirty = false;
}

static void scan_dir(const char *dir, const char *rel, const char *folder, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char full[900], r[700];
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        snprintf(r, sizeof(r), "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4)
                scan_dir(full, r, depth == 0 ? de->d_name : folder, depth + 1);
            continue;
        }
        if (!book_supported(de->d_name))
            continue;
        lib = realloc(lib, (nlib + 1) * sizeof(Entry));
        Entry *e = &lib[nlib++];
        memset(e, 0, sizeof(*e));
        copy_str(e->file, sizeof(e->file), full);
        copy_str(e->rel, sizeof(e->rel), r);
        copy_str(e->folder, sizeof(e->folder), folder);
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->added = st.st_mtime;
        book_guess_from_name(full, e->title, sizeof(e->title), e->author, sizeof(e->author));
    }
    closedir(d);
}

static void load_library(void)
{
    scan_dir(books_dir, "", "", 0);
    char *s = read_all(lib_path);
    cJSON *root = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!root)
        return;
    cJSON *sm = cJSON_GetObjectItem(root, "sort");
    if (cJSON_IsNumber(sm) && sm->valueint >= 0 && sm->valueint < SORT_COUNT)
        sort_mode = sm->valueint;
    cJSON *books = cJSON_GetObjectItem(root, "books");
    for (int i = 0; i < nlib; i++) {
        Entry *e = &lib[i];
        cJSON *o = cJSON_GetObjectItem(books, e->rel);
        if (!o)
            continue;
        cJSON *v;
#define NUM(k) ((v = cJSON_GetObjectItem(o, k)) && cJSON_IsNumber(v) ? v->valuedouble : 0)
        bool same = (long)NUM("size") == e->size && (long)NUM("mtime") == e->mtime;
        if (same && cJSON_IsTrue(cJSON_GetObjectItem(o, "meta"))) {
            if ((v = cJSON_GetObjectItem(o, "title")) && cJSON_IsString(v) && *v->valuestring)
                copy_str(e->title, sizeof(e->title), v->valuestring);
            if ((v = cJSON_GetObjectItem(o, "author")) && cJSON_IsString(v))
                copy_str(e->author, sizeof(e->author), v->valuestring);
            e->meta_done = true;
        }
        if (NUM("added") > 0)
            e->added = (long)NUM("added");
        // reading position survives a replaced file only when it is the same size
        if (same) {
            e->chapter = (int)NUM("chapter");
            e->offset = (int)NUM("offset");
        }
        e->progress = NUM("progress");
        e->last_read = (long)NUM("last");
        e->finished = cJSON_IsTrue(cJSON_GetObjectItem(o, "finished"));
#undef NUM
    }
    cJSON_Delete(root);
}

/* ---------- covers ---------- */

static SDL_Surface *new_surface(int w, int h)
{
    return SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
}

// Area-average downscale of RGB pixels (cropped to the target aspect) into a new surface
static SDL_Surface *box_scale(const unsigned char *px, int w, int h, int dw, int dh, bool crop)
{
    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop) {
        if ((long)cw * dh > (long)ch * dw) {
            cw = ch * dw / dh;
            cx = (w - cw) / 2;
        }
        else {
            ch = cw * dh / dw;
            cy = (h - ch) / 2;
        }
    }
    SDL_Surface *s = new_surface(dw, dh);
    if (!s)
        return NULL;
    SDL_LockSurface(s);
    for (int y = 0; y < dh; y++) {
        int y0 = cy + y * ch / dh, y1 = cy + (y + 1) * ch / dh;
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = cx + x * cw / dw, x1 = cx + (x + 1) * cw / dw;
            if (x1 <= x0)
                x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            // sample at most 8x8 points per output pixel to bound the cost on big covers
            int sy = (y1 - y0 + 7) / 8, sx = (x1 - x0 + 7) / 8;
            for (int yy = y0; yy < y1 && yy < h; yy += sy)
                for (int xx = x0; xx < x1 && xx < w; xx += sx) {
                    const unsigned char *p = px + ((long)yy * w + xx) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            if (!n)
                n = 1;
            ((Uint32 *)((Uint8 *)s->pixels + y * s->pitch))[x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    SDL_UnlockSurface(s);
    return s;
}

// A generated cover for books without one: warm colour from the title, title and author in text
static SDL_Surface *make_cover(const char *title, const char *author)
{
    static const SDL_Color palette[] = {{120, 62, 40}, {54, 84, 96}, {84, 64, 110}, {48, 96, 72}, {130, 92, 40}, {104, 48, 60}, {62, 70, 110}};
    SDL_Color bg = palette[hash_str(title, 0) % (sizeof(palette) / sizeof(palette[0]))];
    SDL_Surface *s = new_surface(DETAIL_W, DETAIL_H), *saved = screen;
    if (!s)
        return NULL;
    screen = s;
    fill(0, 0, DETAIL_W, DETAIL_H, bg);
    SDL_Color dark = {bg.r * 3 / 4, bg.g * 3 / 4, bg.b * 3 / 4};
    fill(0, 0, 10, DETAIL_H, dark);
    fill(22, 26, DETAIL_W - 44, 2, C_ACCENT);
    int y = text_wrap(f24, title, 22, 40, DETAIL_W - 40, 28, 5, C_WHITE);
    fill(22, y + 10, 40, 2, C_ACCENT);
    if (*author)
        text_wrap(f15, author, 22, DETAIL_H - 54, DETAIL_W - 40, 19, 2, C_SOFT);
    screen = saved;
    return s;
}

static void cover_path(Entry *e, char *out, size_t n)
{
    snprintf(out, n, "%s/%08lx.bmp", cover_dir, hash_str(e->rel, e->size));
}

static void set_cover(Entry *e, SDL_Surface *img)
{
    e->card = scale_rounded(img, CARD_W, CARD_H, 8);
    e->big = scale_rounded(img, DETAIL_W, DETAIL_H, 12);
    e->cover_state = 1;
}

// Fill in metadata and the cover. Does the slow work (opening the book) only once per book.
static void load_entry(Entry *e)
{
    char cp[500];
    cover_path(e, cp, sizeof(cp));
    if (e->meta_done && e->cover_state == 0) {
        SDL_Surface *img = SDL_LoadBMP(cp);
        if (img) {
            set_cover(e, img);
            SDL_FreeSurface(img);
            return;
        }
    }
    Book *b = book_open(e->file, true);
    SDL_Surface *img = NULL;
    if (b) {
        copy_str(e->title, sizeof(e->title), b->title);
        copy_str(e->author, sizeof(e->author), b->author);
        long len = 0;
        unsigned char *data = book_image(b, b->cover, &len);
        if (data) {
            int w, h, comp;
            unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
            if (px) {
                img = box_scale(px, w, h, DETAIL_W, DETAIL_H, true);
                stbi_image_free(px);
            }
            free(data);
        }
        book_close(b);
    }
    if (!img)
        img = make_cover(e->title, e->author);
    if (img) {
        SDL_SaveBMP(img, cp);
        set_cover(e, img);
        SDL_FreeSurface(img);
    }
    else
        e->cover_state = 1;
    e->meta_done = true;
    mark_lib_dirty();
}

/* ---------- shelf views ---------- */

static int cmp_str(const char *a, const char *b)
{
    // "The Hobbit" sorts with H
    if (!strncasecmp(a, "the ", 4))
        a += 4;
    if (!strncasecmp(b, "the ", 4))
        b += 4;
    return strcasecmp(a, b);
}

static int cmp_entries(const void *pa, const void *pb)
{
    const Entry *a = &lib[*(const int *)pa], *b = &lib[*(const int *)pb];
    int kind = tabs[tab_sel].kind;
    if (kind == 1 || sort_mode == SORT_RECENT) {
        if (a->last_read != b->last_read)
            return a->last_read > b->last_read ? -1 : 1;
    }
    else if (sort_mode == SORT_AUTHOR) {
        int c = cmp_str(a->author, b->author);
        if (c)
            return c;
    }
    else if (sort_mode == SORT_ADDED) {
        if (a->added != b->added)
            return a->added > b->added ? -1 : 1;
    }
    return cmp_str(a->title, b->title);
}

static void build_tabs(void)
{
    int old_kind = ntabs ? tabs[tab_sel].kind : -1;
    char old_name[96];
    copy_str(old_name, sizeof(old_name), ntabs ? tabs[tab_sel].name : "");
    ntabs = 0;
    bool reading = false, finished = false;
    for (int i = 0; i < nlib; i++) {
        if (lib[i].finished)
            finished = true;
        else if (lib[i].last_read > 0)
            reading = true;
    }
    if (reading) {
        copy_str(tabs[ntabs].name, sizeof(tabs[0].name), "Reading now");
        tabs[ntabs++].kind = 1;
    }
    copy_str(tabs[ntabs].name, sizeof(tabs[0].name), "All");
    tabs[ntabs++].kind = 0;
    for (int i = 0; i < nlib && ntabs < MAX_TABS - 1; i++) {
        if (!*lib[i].folder)
            continue;
        bool seen = false;
        for (int t = 0; t < ntabs; t++)
            if (tabs[t].kind == 3 && !strcmp(tabs[t].name, lib[i].folder))
                seen = true;
        if (!seen) {
            copy_str(tabs[ntabs].name, sizeof(tabs[0].name), lib[i].folder);
            tabs[ntabs++].kind = 3;
        }
    }
    if (finished) {
        copy_str(tabs[ntabs].name, sizeof(tabs[0].name), "Finished");
        tabs[ntabs++].kind = 2;
    }
    tab_sel = 0;
    for (int t = 0; t < ntabs; t++)
        if (tabs[t].kind == old_kind && (old_kind != 3 || !strcmp(tabs[t].name, old_name)))
            tab_sel = t;
}

static void build_view(void)
{
    Entry *keep = nview && view_sel < nview ? &lib[view[view_sel]] : NULL;
    free(view);
    view = malloc((nlib + 1) * sizeof(int));
    nview = 0;
    Tab *t = &tabs[tab_sel];
    for (int i = 0; i < nlib; i++) {
        Entry *e = &lib[i];
        bool ok = t->kind == 0 || (t->kind == 1 && e->last_read > 0 && !e->finished) || (t->kind == 2 && e->finished) ||
                  (t->kind == 3 && !strcmp(e->folder, t->name));
        if (ok)
            view[nview++] = i;
    }
    qsort(view, nview, sizeof(int), cmp_entries);
    view_sel = 0;
    for (int i = 0; i < nview; i++)
        if (&lib[view[i]] == keep)
            view_sel = i;
    view_scroll = 0;
    dirty = true;
}

static void refresh_shelf(void)
{
    build_tabs();
    build_view();
}

/* ---------- reader settings ---------- */

typedef struct {
    int size, font, theme, spacing, margin;
    bool justify;
} Settings;

static Settings set = {3, 0, 0, 1, 1, true};
static const int font_sizes[] = {16, 18, 20, 23, 26, 30};
#define NSIZES (int)(sizeof(font_sizes) / sizeof(font_sizes[0]))
static const char *font_names[] = {"Caladea", "DejaVu Sans"};
// [font][style]; NULL = slant/embolden the regular face
static const char *font_files[2][4] = {{"Caladea-Regular.ttf", "Caladea-Bold.ttf", "Caladea-Italic.ttf", "Caladea-BoldItalic.ttf"},
                                       {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", NULL, NULL}};
static const char *theme_names[] = {"Dark", "Sepia", "Light", "Night"};
static const SDL_Color theme_bg[] = {{37, 32, 29}, {241, 230, 207}, {250, 249, 245}, {0, 0, 0}};
static const SDL_Color theme_fg[] = {{228, 218, 206}, {62, 46, 30}, {28, 28, 28}, {165, 155, 145}};
static const SDL_Color theme_dim[] = {{140, 128, 116}, {140, 118, 92}, {130, 130, 130}, {85, 80, 75}};
static const char *spacing_names[] = {"Tight", "Normal", "Airy"};
static const float spacing_mul[] = {1.05f, 1.25f, 1.5f};
static const char *margin_names[] = {"Narrow", "Normal", "Wide"};
static const int margin_px[] = {14, 30, 60};

static void save_settings(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "size", set.size);
    cJSON_AddNumberToObject(o, "font", set.font);
    cJSON_AddNumberToObject(o, "theme", set.theme);
    cJSON_AddNumberToObject(o, "spacing", set.spacing);
    cJSON_AddNumberToObject(o, "margin", set.margin);
    cJSON_AddBoolToObject(o, "justify", set.justify);
    char *s = cJSON_Print(o);
    cJSON_Delete(o);
    FILE *fp = s ? fopen(settings_path, "w") : NULL;
    if (fp) {
        fputs(s, fp);
        fclose(fp);
    }
    free(s);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi
                                                                       : v; }

static void load_settings(void)
{
    char *s = read_all(settings_path);
    cJSON *o = s ? cJSON_Parse(s) : NULL;
    free(s);
    if (!o)
        return;
    cJSON *v;
#define GET(k, lo, hi)                                         \
    if ((v = cJSON_GetObjectItem(o, #k)) && cJSON_IsNumber(v)) \
    set.k = clampi(v->valueint, lo, hi)
    GET(size, 0, NSIZES - 1);
    GET(font, 0, 1);
    GET(theme, 0, 3);
    GET(spacing, 0, 2);
    GET(margin, 0, 2);
#undef GET
    if ((v = cJSON_GetObjectItem(o, "justify")))
        set.justify = cJSON_IsTrue(v);
    cJSON_Delete(o);
}

/* ---------- reader: fonts ---------- */

// [heading class][style]: class 0 = body, 1 = h1, 2 = h2, 3 = h3..h6
static TTF_Font *rf[4][4];
static int rf_size = -1, rf_font = -1;

static void close_reader_fonts(void)
{
    for (int c = 0; c < 4; c++)
        for (int s = 0; s < 4; s++) {
            if (rf[c][s])
                TTF_CloseFont(rf[c][s]);
            rf[c][s] = NULL;
        }
}

static TTF_Font *rfont(int hclass, int style)
{
    if (rf_size != set.size || rf_font != set.font) {
        close_reader_fonts();
        rf_size = set.size;
        rf_font = set.font;
    }
    if (!rf[hclass][style]) {
        static const float mul[] = {1.0f, 1.5f, 1.3f, 1.12f};
        char path[500];
        int px = (int)(font_sizes[set.size] * mul[hclass] + 0.5f);
        const char *file = font_files[set.font][style];
        int synth = 0;
        if (!file) {
            // closest real face, the rest synthesized
            file = font_files[set.font][style & ST_BOLD];
            if (!file) {
                file = font_files[set.font][0];
                synth |= style & ST_BOLD ? TTF_STYLE_BOLD : 0;
            }
            synth |= style & ST_ITALIC ? TTF_STYLE_ITALIC : 0;
        }
        snprintf(path, sizeof(path), "%s/%s", font_dir, file);
        TTF_Font *f = TTF_OpenFont(path, px);
        if (!f)
            f = TTF_OpenFont(ui_font, px);
        if (f && synth)
            TTF_SetFontStyle(f, synth);
        rf[hclass][style] = f;
    }
    return rf[hclass][style];
}

/* ---------- reader: layout ---------- */

typedef struct {
    int off, len;
    short x, w;
    unsigned char style;
} Word;

typedef struct {
    int first, count; // words
    int off;          // text offset of the line start
    short y, h;
    unsigned char kind; // B_TEXT, B_IMAGE, B_RULE
    unsigned char hclass;
    short img; // index into images
} Line;

typedef struct {
    char src[512];
    int w, h;
    SDL_Surface *surf;
} Img;

static Book *book = NULL;
static Entry *reading = NULL;
static Doc *doc = NULL;
static int chapter = 0;
static Word *words = NULL;
static int nwords = 0, cap_words = 0;
static Line *lines = NULL;
static int nlines = 0, cap_lines = 0;
static int *pages = NULL; // first line of each page
static int npages = 0, page = 0;
static Img *imgs = NULL;
static int nimgs = 0;
static SDL_Surface *page_cache = NULL;
static bool page_dirty = true;
static int text_x, text_w_px, text_y, text_h;
enum { OV_NONE,
       OV_SETTINGS,
       OV_TOC };
static int overlay = OV_NONE, ov_sel = 0, ov_scroll = 0;

static void free_layout(void)
{
    for (int i = 0; i < nimgs; i++)
        if (imgs[i].surf)
            SDL_FreeSurface(imgs[i].surf);
    free(imgs);
    imgs = NULL;
    nimgs = 0;
    nwords = nlines = npages = 0;
}

static Word *push_word(void)
{
    if (nwords + 1 > cap_words) {
        cap_words = cap_words ? cap_words * 2 : 4096;
        words = realloc(words, cap_words * sizeof(Word));
    }
    return &words[nwords++];
}

static Line *push_line(void)
{
    if (nlines + 1 > cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 1024;
        lines = realloc(lines, cap_lines * sizeof(Line));
    }
    Line *l = &lines[nlines++];
    memset(l, 0, sizeof(*l));
    l->first = nwords;
    return l;
}

static int measure(TTF_Font *f, const char *s, int len)
{
    char buf[512];
    if (len > 511)
        len = 511;
    memcpy(buf, s, len);
    buf[len] = '\0';
    int w = 0, h = 0;
    TTF_SizeUTF8(f, buf, &w, &h);
    return w;
}

static int hclass_of(int heading) { return heading == 0 ? 0 : heading == 1 ? 1
                                                          : heading == 2   ? 2
                                                                           : 3; }

// Spread the free space of a full line over its word gaps
static void justify_line(Line *l, int width, int indent_px)
{
    if (l->count < 2)
        return;
    Word *last = &words[l->first + l->count - 1];
    int used = last->x + last->w - indent_px;
    int extra = width - indent_px - used;
    if (extra <= 0 || extra > width / 3)
        return;
    int gaps = 0;
    for (int i = 1; i < l->count; i++)
        if (words[l->first + i].x > words[l->first + i - 1].x + words[l->first + i - 1].w)
            gaps++;
    if (!gaps)
        return;
    int shift = 0, g = 0;
    for (int i = 1; i < l->count; i++) {
        Word *w = &words[l->first + i];
        if (w->x > words[l->first + i - 1].x + words[l->first + i - 1].w - shift) {
            g++;
            shift = extra * g / gaps;
        }
        w->x += shift;
    }
}

static void center_line(Line *l, int width)
{
    if (!l->count)
        return;
    Word *last = &words[l->first + l->count - 1];
    int used = last->x + last->w - words[l->first].x;
    int dx = (width - used) / 2 - words[l->first].x;
    for (int i = 0; i < l->count; i++)
        words[l->first + i].x += dx;
}

static void layout_image(Block *b, int line_h)
{
    long len = 0;
    unsigned char *data = book_image(book, b->src, &len);
    int w = 0, h = 0, comp = 0;
    if (!data || !stbi_info_from_memory(data, (int)len, &w, &h, &comp) || w <= 0 || h <= 0) {
        free(data);
        return;
    }
    free(data);
    // fit to the text area; tiny decorations keep their size
    double sc = 1.0;
    if (w > text_w_px)
        sc = (double)text_w_px / w;
    if (h * sc > text_h - line_h)
        sc = (double)(text_h - line_h) / h;
    if (w < 40 && h < 40)
        sc = 1.0;
    imgs = realloc(imgs, (nimgs + 1) * sizeof(Img));
    Img *im = &imgs[nimgs];
    memset(im, 0, sizeof(*im));
    copy_str(im->src, sizeof(im->src), b->src);
    im->w = (int)(w * sc);
    im->h = (int)(h * sc);
    if (im->w < 1 || im->h < 1)
        return;
    Line *l = push_line();
    l->kind = B_IMAGE;
    l->img = nimgs++;
    l->off = b->off;
    l->h = im->h + line_h / 2;
}

static void layout_chapter(void)
{
    free_layout();
    int margin = margin_px[set.margin];
    text_x = margin;
    text_w_px = SCREEN_W - margin * 2;
    text_y = 16;
    text_h = SCREEN_H - 16 - 34;
    if (!doc)
        return;
    TTF_Font *body = rfont(0, 0);
    int body_h = (int)(TTF_FontLineSkip(body) * spacing_mul[set.spacing] + 0.5f);
    int para_gap = body_h / 2;
    // gap above each line, so it can be dropped at the top of a page
    int *gaps = NULL;
    int cap_gaps = 0;
#define ADD_GAP(g)                                        \
    do {                                                  \
        if (nlines > cap_gaps) {                          \
            cap_gaps = nlines * 2;                        \
            gaps = realloc(gaps, cap_gaps * sizeof(int)); \
        }                                                 \
        gaps[nlines - 1] = (g);                           \
    } while (0)

    for (int bi = 0; bi < doc->nblocks; bi++) {
        Block *b = &doc->blocks[bi];
        int gap = b->cont ? 0 : para_gap;
        if (b->type == B_RULE) {
            Line *l = push_line();
            l->kind = B_RULE;
            l->off = b->off;
            l->h = body_h;
            ADD_GAP(gap);
            continue;
        }
        if (b->type == B_IMAGE) {
            int before = nlines;
            layout_image(b, body_h);
            if (nlines > before)
                ADD_GAP(gap);
            continue;
        }
        int hc = hclass_of(b->heading);
        if (hc)
            gap = body_h;
        TTF_Font *f0 = rfont(hc, b->heading ? ST_BOLD : 0);
        int lh = hc ? (int)(TTF_FontLineSkip(f0) * 1.15f) : body_h;
        int indent = b->indent * 24;
        if (indent > text_w_px / 3)
            indent = text_w_px / 3;
        int width = text_w_px;
        Line *l = push_line();
        l->kind = B_TEXT;
        l->hclass = hc;
        l->h = lh;
        l->off = b->off;
        ADD_GAP(gap);
        int x = indent;
        bool pending_space = false;
        for (int r = b->run_first; r < b->run_first + b->run_count; r++) {
            Run *run = &doc->runs[r];
            unsigned char style = run->style | (b->heading ? ST_BOLD : 0);
            TTF_Font *f = rfont(hc, style & 3);
            // synthesized italics report a wider box than they advance: measure upright
            if ((style & ST_ITALIC) && !font_files[set.font][style & 3])
                f = rfont(hc, style & ST_BOLD);
            int space_w = measure(f, " ", 1);
            const char *s = doc->text + run->start, *e = s + run->len;
            while (s < e) {
                if (*s == ' ') {
                    pending_space = true;
                    s++;
                    continue;
                }
                const char *ws = s;
                while (s < e && *s != ' ')
                    s++;
                int len = (int)(s - ws);
                int w = measure(f, ws, len);
                int sp = pending_space ? space_w : 0;
                if (x + sp + w > width && l->count > 0) {
                    if (set.justify && !b->center)
                        justify_line(l, width, indent);
                    if (b->center)
                        center_line(l, width);
                    l = push_line();
                    l->kind = B_TEXT;
                    l->hclass = hc;
                    l->h = lh;
                    l->off = (int)(ws - doc->text);
                    ADD_GAP(0);
                    x = indent;
                    sp = 0;
                }
                // a word wider than the line: cut it into pieces that fit
                while (w > width - indent && len > 1) {
                    int cut = len;
                    while (cut > 1 && measure(f, ws, cut) > width - x) {
                        do {
                            cut--;
                        } while (cut > 1 && ((unsigned char)ws[cut] & 0xC0) == 0x80);
                    }
                    Word *wd = push_word();
                    wd->off = (int)(ws - doc->text);
                    wd->len = cut;
                    wd->x = x;
                    wd->w = measure(f, ws, cut);
                    wd->style = style & 3;
                    l->count++;
                    ws += cut;
                    len -= cut;
                    w = measure(f, ws, len);
                    l = push_line();
                    l->kind = B_TEXT;
                    l->hclass = hc;
                    l->h = lh;
                    l->off = (int)(ws - doc->text);
                    ADD_GAP(0);
                    x = indent;
                    sp = 0;
                }
                Word *wd = push_word();
                wd->off = (int)(ws - doc->text);
                wd->len = len;
                wd->x = x + sp;
                wd->w = w;
                wd->style = style & 3;
                l->count++;
                x += sp + w;
                pending_space = false;
            }
        }
        if (b->center)
            center_line(l, width);
    }

    // paginate
    free(pages);
    pages = malloc((nlines + 1) * sizeof(int));
    npages = 0;
    int y = 0;
    for (int i = 0; i < nlines; i++) {
        int g = gaps[i];
        // headings start a new page when they would land in the bottom third
        bool heading_low = lines[i].hclass && lines[i].hclass < 3 && y > text_h * 2 / 3 && g > 0;
        if (npages == 0 || y + g + lines[i].h > text_h || heading_low) {
            pages[npages++] = i;
            y = 0;
            g = 0;
        }
        lines[i].y = y + g;
        y += g + lines[i].h;
    }
    if (npages == 0)
        pages[npages++] = 0;
    free(gaps);
#undef ADD_GAP
}

static int page_offset(int p)
{
    if (p < 0 || p >= npages || pages[p] >= nlines)
        return 0;
    return lines[pages[p]].off;
}

static int page_of_offset(int off)
{
    int p = 0;
    for (int i = 0; i < npages; i++)
        if (page_offset(i) <= off)
            p = i;
    return p;
}

/* ---------- reader: state ---------- */

static double book_progress(void)
{
    if (!book || !book->total_weight)
        return 0;
    long before = 0;
    for (int i = 0; i < chapter && i < book->nchapters; i++)
        before += book->chapters[i].weight;
    double in = npages > 1 ? (double)page / (npages - 1) : 1.0;
    if (chapter == book->nchapters - 1 && page == npages - 1)
        return 1.0;
    // the first page of a chapter counts as its start
    if (page == 0)
        in = 0;
    return (before + book->chapters[chapter].weight * in) / (double)book->total_weight;
}

static void remember_position(void)
{
    if (!reading)
        return;
    reading->chapter = chapter;
    reading->offset = page_offset(page);
    reading->progress = book_progress();
    reading->last_read = (long)time(NULL);
    if (reading->progress >= 0.999)
        reading->finished = true;
    mark_lib_dirty();
}

static void open_chapter(int index, int offset, bool last_page)
{
    doc_free(doc);
    chapter = clampi(index, 0, book->nchapters - 1);
    doc = book_chapter(book, chapter);
    layout_chapter();
    if (last_page)
        page = npages - 1;
    else
        page = page_of_offset(offset);
    page_dirty = dirty = true;
}

static void relayout_keep_position(void)
{
    int off = page_offset(page);
    layout_chapter();
    page = page_of_offset(off);
    page_dirty = dirty = true;
}

static void draw_loading(const char *what);

static void start_reading(Entry *e, bool from_start)
{
    draw_loading("Opening the book …");
    book = book_open(e->file, false);
    if (!book) {
        show_message("Could not open the book", 2500);
        return;
    }
    reading = e;
    if (from_start || e->finished) {
        e->chapter = 0;
        e->offset = 0;
        e->finished = false;
    }
    open_chapter(e->chapter, e->offset, false);
    overlay = OV_NONE;
    scr = S_READ;
    remember_position();
}

static void stop_reading(void)
{
    remember_position();
    save_library();
    doc_free(doc);
    doc = NULL;
    free_layout();
    book_close(book);
    book = NULL;
    reading = NULL;
    close_reader_fonts();
    refresh_shelf();
}

static void next_page(void)
{
    if (page + 1 < npages)
        page++;
    else if (chapter + 1 < book->nchapters)
        open_chapter(chapter + 1, 0, false);
    else {
        if (!reading->finished)
            show_message("The end. The book is marked as finished.", 2500);
        reading->finished = true;
    }
    page_dirty = dirty = true;
    remember_position();
}

static void prev_page(void)
{
    if (page > 0)
        page--;
    else if (chapter > 0)
        open_chapter(chapter - 1, 0, true);
    page_dirty = dirty = true;
    remember_position();
}

/* ---------- drawing: reader ---------- */

static SDL_Surface *get_image(Img *im)
{
    if (im->surf)
        return im->surf;
    long len = 0;
    unsigned char *data = book_image(book, im->src, &len);
    if (!data)
        return NULL;
    int w, h, comp;
    unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
    free(data);
    if (!px)
        return NULL;
    if (im->w >= w) {
        // no downscale needed: copy straight in (upscale with the bilinear helper)
        SDL_Surface *s = box_scale(px, w, h, w, h, false);
        if (s && (im->w != w || im->h != h)) {
            SDL_Surface *big = scale_rounded(s, im->w, im->h, 0);
            SDL_FreeSurface(s);
            s = big;
        }
        im->surf = s;
    }
    else
        im->surf = box_scale(px, w, h, im->w, im->h, false);
    stbi_image_free(px);
    return im->surf;
}

static void render_page(void)
{
    if (!page_cache)
        page_cache = new_surface(SCREEN_W, SCREEN_H);
    SDL_Surface *saved = screen;
    screen = page_cache;
    SDL_Color bg = theme_bg[set.theme], fg = theme_fg[set.theme], dim = theme_dim[set.theme];
    fill(0, 0, SCREEN_W, SCREEN_H, bg);
    int first = page < npages ? pages[page] : 0;
    int last = page + 1 < npages ? pages[page + 1] : nlines;
    for (int i = first; i < last; i++) {
        Line *l = &lines[i];
        int y = text_y + l->y;
        if (l->kind == B_RULE) {
            fill(text_x + text_w_px / 2 - 40, y + l->h / 2, 80, 1, dim);
            continue;
        }
        if (l->kind == B_IMAGE) {
            Img *im = &imgs[l->img];
            SDL_Surface *s = get_image(im);
            if (s) {
                SDL_Rect pos = {text_x + (text_w_px - im->w) / 2, y + (l->h - im->h) / 2};
                SDL_BlitSurface(s, NULL, screen, &pos);
            }
            continue;
        }
        for (int k = 0; k < l->count; k++) {
            Word *w = &words[l->first + k];
            char buf[512];
            int len = w->len > 511 ? 511 : w->len;
            memcpy(buf, doc->text + w->off, len);
            buf[len] = '\0';
            TTF_Font *f = rfont(l->hclass, w->style);
            SDL_Surface *t = TTF_RenderUTF8_Blended(f, buf, l->hclass ? (set.theme == 0 ? C_ACCENT : fg) : fg);
            if (t) {
                SDL_Rect pos = {text_x + w->x, y};
                SDL_BlitSurface(t, NULL, screen, &pos);
                SDL_FreeSurface(t);
            }
        }
    }
    if (doc && doc->nblocks == 0)
        text(f17, "(Empty page)", (SCREEN_W - text_w(f17, "(Empty page)")) / 2, 200, dim);

    // status line
    char ch[200], pos[64];
    book_chapter_title(book, chapter, ch, sizeof(ch));
    snprintf(pos, sizeof(pos), "%d / %d   ·   %d %%", page + 1, npages, (int)(book_progress() * 100 + 0.5));
    int pw = text_w(f13, pos);
    text_fit(f13, ch, text_x, SCREEN_H - 26, SCREEN_W - text_x * 2 - pw - 20, dim);
    text(f13, pos, SCREEN_W - text_x - pw, SCREEN_H - 26, dim);
    int bar_w = SCREEN_W - text_x * 2;
    fill(text_x, SCREEN_H - 31, bar_w, 1, (SDL_Color){(bg.r + dim.r) / 2, (bg.g + dim.g) / 2, (bg.b + dim.b) / 2});
    fill(text_x, SCREEN_H - 31, (int)(bar_w * book_progress()), 1, set.theme == 0 ? C_ACCENT : dim);
    screen = saved;
    page_dirty = false;
}

static void draw_panel_rows(const char *title, int x, int w, int n, int sel, int scroll, int visible,
                            void (*row)(int i, int x, int y, int w, bool s))
{
    fill(x, 0, w, SCREEN_H, C_BG);
    fill(x, 0, 3, SCREEN_H, C_ACCENT);
    fill(x, 0, w, TOP_H, C_BAR);
    text(f24, title, x + 22, 11, C_WHITE);
    for (int k = 0; k < visible && scroll + k < n; k++) {
        int i = scroll + k;
        int y = TOP_H + 10 + k * 46;
        bool s = i == sel;
        if (s) {
            fill_round(x + 12, y, w - 24, 40, 10, C_CARD);
            fill_round(x + 12, y, 5, 40, 2, C_ACCENT);
        }
        row(i, x + 26, y, w - 50, s);
    }
}

#define NSET 6
static void settings_row(int i, int x, int y, int w, bool s)
{
    static const char *labels[NSET] = {"Text size", "Font", "Theme", "Line spacing", "Margins", "Justify"};
    char val[40];
    switch (i) {
    case 0:
        snprintf(val, sizeof(val), "%d", font_sizes[set.size]);
        break;
    case 1:
        copy_str(val, sizeof(val), font_names[set.font]);
        break;
    case 2:
        copy_str(val, sizeof(val), theme_names[set.theme]);
        break;
    case 3:
        copy_str(val, sizeof(val), spacing_names[set.spacing]);
        break;
    case 4:
        copy_str(val, sizeof(val), margin_names[set.margin]);
        break;
    default:
        copy_str(val, sizeof(val), set.justify ? "On" : "Off");
    }
    text(f17, labels[i], x, y + 9, s ? C_WHITE : C_SOFT);
    char shown[60];
    snprintf(shown, sizeof(shown), s ? "‹  %s  ›" : "%s", val);
    text(f17, shown, x + w - text_w(f17, shown), y + 9, s ? C_ACCENT : C_GREY);
}

static void toc_row(int i, int x, int y, int w, bool s)
{
    char t[200];
    int ch = book->ntoc ? book->toc[i].chapter : i;
    if (book->ntoc)
        copy_str(t, sizeof(t), book->toc[i].title);
    else
        snprintf(t, sizeof(t), "Part %d", i + 1);
    bool current = ch == chapter || (book->ntoc && ch < chapter && (i + 1 >= book->ntoc || book->toc[i + 1].chapter > chapter));
    text_fit(f17, t, x, y + 9, w - 20, current ? C_ACCENT : (s ? C_WHITE : C_SOFT));
    if (current)
        text(f15, "●", x + w - 10, y + 10, C_ACCENT);
}

static int toc_count(void) { return book->ntoc ? book->ntoc : book->nchapters; }

static void draw_reader(void)
{
    if (page_dirty || !page_cache)
        render_page();
    SDL_BlitSurface(page_cache, NULL, screen, NULL);
    if (overlay == OV_SETTINGS) {
        draw_panel_rows("Reading", SCREEN_W - 330, 330, NSET, ov_sel, 0, NSET, settings_row);
        int x = SCREEN_W - 330;
        fill(x, BOTTOM_Y, 330, SCREEN_H - BOTTOM_Y, C_BAR);
        fill(x, BOTTOM_Y, 3, SCREEN_H - BOTTOM_Y, C_ACCENT);
        int hx = x + 20;
        const char *keys[] = {"←→", "B"}, *labels[] = {"Change", "Close"};
        for (int i = 0; i < 2; i++) {
            int kw = text_w(f15, keys[i]);
            int bw = kw + 12 < 22 ? 22 : kw + 12;
            fill_round(hx, 453, bw, 22, 11, C_ACCENT);
            text(f15, keys[i], hx + (bw - kw) / 2, 454, C_DARK);
            text(f15, labels[i], hx + bw + 6, 453, C_WHITE);
            hx += bw + 6 + text_w(f15, labels[i]) + 20;
        }
        // live preview line
        text(f13, "Changes show on the page right away.", x + 26, TOP_H + 10 + NSET * 46 + 8, C_GREY);
    }
    else if (overlay == OV_TOC) {
        int n = toc_count(), visible = 8;
        if (ov_sel < ov_scroll)
            ov_scroll = ov_sel;
        if (ov_sel >= ov_scroll + visible)
            ov_scroll = ov_sel - visible + 1;
        draw_panel_rows("Contents", SCREEN_W - 400, 400, n, ov_sel, ov_scroll, visible, toc_row);
        int x = SCREEN_W - 400;
        fill(x, BOTTOM_Y, 400, SCREEN_H - BOTTOM_Y, C_BAR);
        fill(x, BOTTOM_Y, 3, SCREEN_H - BOTTOM_Y, C_ACCENT);
        int hx = x + 20;
        const char *keys[] = {"A", "B"}, *labels[] = {"Go to", "Close"};
        for (int i = 0; i < 2; i++) {
            int kw = text_w(f15, keys[i]);
            int bw = kw + 12 < 22 ? 22 : kw + 12;
            fill_round(hx, 453, bw, 22, 11, C_ACCENT);
            text(f15, keys[i], hx + (bw - kw) / 2, 454, C_DARK);
            text(f15, labels[i], hx + bw + 6, 453, C_WHITE);
            hx += bw + 6 + text_w(f15, labels[i]) + 20;
        }
        if (n > visible) {
            // scroll indicator
            int track = visible * 46, th = track * visible / n;
            int ty = TOP_H + 10 + (track - th) * ov_scroll / (n - visible);
            fill_round(SCREEN_W - 8, ty, 4, th, 2, C_CARD);
        }
    }
}

/* ---------- drawing: shelf ---------- */

static void draw_topbar(void)
{
    soup_logo("Books");
    if (scr != S_SHELF || ntabs == 0)
        return;
    int tx = SCREEN_W - 16;
    for (int i = ntabs - 1; i >= 0; i--)
        tx -= text_w(f17, tabs[i].name) + (i ? 26 : 0);
    // too many tabs: start at the selected one
    int first = 0;
    if (tx < 190) {
        tx = 190;
        first = tab_sel > 0 ? tab_sel - 1 : 0;
    }
    for (int i = first; i < ntabs; i++) {
        int w = text_w(f17, tabs[i].name);
        if (tx + w > SCREEN_W - 6)
            break;
        if (i == tab_sel) {
            fill_round(tx - 10, 12, w + 20, 28, 14, C_ACCENT);
            text(f17, tabs[i].name, tx, 15, C_DARK);
        }
        else
            text(f17, tabs[i].name, tx, 15, C_GREY);
        tx += w + 26;
    }
}

static void draw_hint_bar(void)
{
    const char *k[5], *l[5];
    int n = 0;
#define HINT(a, b) (k[n] = (a), l[n] = (b), n++)
    if (scr == S_SHELF) {
        if (nview)
            HINT("A", "Open");
        HINT("B", "Exit");
        if (ntabs > 1)
            HINT("L/R", "Shelf");
        if (tabs[tab_sel].kind != 1)
            HINT("X", "Sort");
        bool any = false;
        for (int i = 0; i < nlib; i++)
            if (lib[i].last_read > 0 && !lib[i].finished)
                any = true;
        if (any)
            HINT("Y", "Continue");
    }
    else if (scr == S_DETAIL) {
        bool started = detail->last_read > 0 && !detail->finished && detail->progress > 0;
        HINT("A", started ? "Continue" : "Read");
        if (started)
            HINT("X", "From start");
        HINT("Y", detail->finished ? "Mark as unread" : "Mark as read");
        HINT("B", "Back");
    }
    draw_hints(k, l, n);
#undef HINT
}

static void draw_card(Entry *e, int x, int y, bool selected)
{
    if (selected)
        outline_round(x - 5, y - 5, CARD_W + 10, CARD_H + 10, 12, 3, C_ACCENT, C_BG);
    if (e->card) {
        SDL_Rect pos = {x, y};
        SDL_BlitSurface(e->card, NULL, screen, &pos);
    }
    else {
        fill_round(x, y, CARD_W, CARD_H, 8, C_CARD);
        text_wrap(f13, e->title, x + 10, y + 12, CARD_W - 20, 16, 5, C_GREY);
    }
    if (e->finished) {
        fill_round(x + CARD_W - 30, y + 5, 25, 18, 9, C_GREEN);
        text(f13, "✓", x + CARD_W - 22, y + 6, C_WHITE);
    }
    else if (e->progress > 0.001) {
        fill_round(x + 8, y + CARD_H - 12, CARD_W - 16, 5, 2, C_DARK);
        int pw = (int)((CARD_W - 16) * e->progress);
        if (pw > 0)
            fill_round(x + 8, y + CARD_H - 12, pw < 4 ? 4 : pw, 5, 2, C_ACCENT);
    }
    text_fit(f13, e->title, x, y + CARD_H + 6, CARD_W, selected ? C_WHITE : C_GREY);
}

static void draw_center_text(const char *big, const char *small, SDL_Color c)
{
    text(f24, big, (SCREEN_W - text_w(f24, big)) / 2, 180, c);
    if (small)
        text_wrap(f15, small, 90, 222, SCREEN_W - 180, 21, 4, C_GREY);
}

static void draw_shelf(void)
{
    Tab *t = &tabs[tab_sel];
    char head[200], count[120];
    copy_str(head, sizeof(head), t->kind == 0 ? "All books" : t->name);
    text_fit(f19, head, 24, TOP_H + 8, 300, C_WHITE);
    snprintf(count, sizeof(count), "%d %s%s%s", nview, nview == 1 ? "book" : "books", t->kind == 1 ? "" : "  ·  ",
             t->kind == 1 ? "" : sort_names[sort_mode]);
    text(f15, count, SCREEN_W - 24 - text_w(f15, count), TOP_H + 12, C_GREY);
    if (nlib == 0) {
        draw_center_text("No books yet",
                         "Put EPUB or TXT files in the Media/Books folder on the SD card. Subfolders become their own shelves.",
                         C_WHITE);
        return;
    }
    if (nview == 0) {
        draw_center_text("Empty", "No books here.", C_GREY);
        return;
    }
    if (view_sel >= nview)
        view_sel = nview - 1;
    int row = view_sel / 5;
    if (row < view_scroll)
        view_scroll = row;
    if (row > view_scroll + 1)
        view_scroll = row - 1;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 5; c++) {
            int i = (view_scroll + r) * 5 + c;
            if (i >= nview)
                break;
            draw_card(&lib[view[i]], 24 + c * 120, TOP_H + 42 + r * 176, i == view_sel);
        }
    int rows = (nview + 4) / 5;
    if (rows > 2) {
        int track = 352, th = track * 2 / rows;
        int ty = TOP_H + 42 + (track - th) * view_scroll / (rows - 2);
        fill_round(SCREEN_W - 8, ty, 4, th, 2, C_CARD);
    }
}

static void open_detail(Entry *e)
{
    detail = e;
    free(detail_desc);
    detail_desc = NULL;
    detail_chapters = 0;
    Book *b = book_open(e->file, true);
    if (b) {
        if (b->description)
            detail_desc = strdup(b->description);
        detail_chapters = b->nchapters;
        book_close(b);
    }
    scr = S_DETAIL;
    dirty = true;
}

static void draw_detail(void)
{
    Entry *e = detail;
    if (e->big) {
        SDL_Rect pos = {24, TOP_H + 18};
        SDL_BlitSurface(e->big, NULL, screen, &pos);
    }
    else
        fill_round(24, TOP_H + 18, DETAIL_W, DETAIL_H, 12, C_CARD);
    int x = 228, w = SCREEN_W - x - 24;
    int y = text_wrap(f34, e->title, x, TOP_H + 12, w, 38, 2, C_WHITE);
    if (*e->author) {
        text_fit(f19, e->author, x, y, w, C_ACCENT);
        y += 26;
    }
    const char *ext = strrchr(e->file, '.');
    char size[32], meta[160], ext_up[8] = "";
    human_size(e->size, size, sizeof(size));
    for (int i = 0; ext && ext[i + 1] && i < 6; i++)
        ext_up[i] = toupper((unsigned char)ext[i + 1]), ext_up[i + 1] = '\0';
    if (detail_chapters > 1)
        snprintf(meta, sizeof(meta), "%s  ·  %s  ·  %d chapters", ext_up, size, detail_chapters);
    else
        snprintf(meta, sizeof(meta), "%s  ·  %s", ext_up, size);
    text_fit(f15, meta, x, y + 2, w, C_GREY);
    y += 30;
    text_wrap(f15, detail_desc && *detail_desc ? detail_desc : "No description.", x, y, w, 21, 5, C_SOFT);

    int by = TOP_H + 222;
    bool started = e->last_read > 0 && !e->finished && e->progress > 0;
    char label[48];
    if (started)
        snprintf(label, sizeof(label), "Continue  ·  %d %%", (int)(e->progress * 100 + 0.5));
    else
        copy_str(label, sizeof(label), e->finished ? "Read again" : "Read");
    int lw = text_w(f19, label) + 40;
    fill_round(x, by, lw, 36, 18, C_ACCENT);
    text(f19, label, x + 20, by + 6, C_DARK);
    if (started) {
        int pw = lw - 36;
        fill_round(x + 18, by + 42, pw, 4, 2, C_CARD);
        int fw = (int)(pw * e->progress);
        if (fw > 0)
            fill_round(x + 18, by + 42, fw, 4, 2, C_ACCENT);
    }
    else if (e->finished)
        text(f15, "Finished", x + lw + 16, by + 9, C_GREEN);
    if (e->last_read > 0) {
        char when[80];
        time_t tt = e->last_read;
        struct tm *tm = localtime(&tt);
        strftime(when, sizeof(when), "Last read %d.%m.%Y", tm);
        text(f13, when, x, by + 56, C_GREY);
    }
}

static void draw_loading(const char *what)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    soup_logo("Books");
    text(f24, what, (SCREEN_W - text_w(f24, what)) / 2, 210, C_WHITE);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

static void draw(void)
{
    if (scr == S_READ)
        draw_reader();
    else {
        SDL_FillRect(screen, NULL, map(screen, C_BG));
        if (scr == S_SHELF)
            draw_shelf();
        else
            draw_detail();
        draw_topbar();
        draw_hint_bar();
    }
    if (*message && SDL_GetTicks() < message_until) {
        int w = text_w(f17, message) + 40;
        if (w > SCREEN_W - 40)
            w = SCREEN_W - 40;
        fill_round((SCREEN_W - w) / 2, BOTTOM_Y - 96, w, 40, 20, C_CARD);
        text_fit(f17, message, (SCREEN_W - w) / 2 + 20, BOTTOM_Y - 88, w - 40, C_WHITE);
    }
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
    dirty = false;
}

/* ---------- input ---------- */

static void change_setting(int row, int dir)
{
    switch (row) {
    case 0:
        set.size = clampi(set.size + dir, 0, NSIZES - 1);
        break;
    case 1:
        set.font = (set.font + 1) % 2;
        break;
    case 2:
        set.theme = (set.theme + dir + 4) % 4;
        break;
    case 3:
        set.spacing = clampi(set.spacing + dir, 0, 2);
        break;
    case 4:
        set.margin = clampi(set.margin + dir, 0, 2);
        break;
    default:
        set.justify = !set.justify;
    }
    save_settings();
    if (row == 2)
        page_dirty = true;
    else
        relayout_keep_position();
}

static int toc_index_for_chapter(void)
{
    if (!book->ntoc)
        return chapter;
    int best = 0;
    for (int i = 0; i < book->ntoc; i++)
        if (book->toc[i].chapter <= chapter)
            best = i;
    return best;
}

static void on_key_reader(SDLKey key)
{
    if (overlay == OV_SETTINGS) {
        if (key == SW_BTN_UP)
            ov_sel = (ov_sel + NSET - 1) % NSET;
        else if (key == SW_BTN_DOWN)
            ov_sel = (ov_sel + 1) % NSET;
        else if (key == SW_BTN_LEFT || key == SW_BTN_RIGHT || key == SW_BTN_A)
            change_setting(ov_sel, key == SW_BTN_LEFT ? -1 : 1);
        else if (key == SW_BTN_B || key == SW_BTN_START || key == SW_BTN_MENU)
            overlay = OV_NONE;
        return;
    }
    if (overlay == OV_TOC) {
        int n = toc_count();
        if (key == SW_BTN_UP)
            ov_sel = (ov_sel + n - 1) % n;
        else if (key == SW_BTN_DOWN)
            ov_sel = (ov_sel + 1) % n;
        else if (key == SW_BTN_LEFT)
            ov_sel = clampi(ov_sel - 8, 0, n - 1);
        else if (key == SW_BTN_RIGHT)
            ov_sel = clampi(ov_sel + 8, 0, n - 1);
        else if (key == SW_BTN_A) {
            int ch = book->ntoc ? book->toc[ov_sel].chapter : ov_sel;
            open_chapter(ch, 0, false);
            overlay = OV_NONE;
            remember_position();
        }
        else if (key == SW_BTN_B || key == SW_BTN_SELECT)
            overlay = OV_NONE;
        return;
    }
    if (key == SW_BTN_RIGHT || key == SW_BTN_DOWN || key == SW_BTN_A || key == SW_BTN_R1)
        next_page();
    else if (key == SW_BTN_LEFT || key == SW_BTN_UP || key == SW_BTN_L1)
        prev_page();
    else if (key == SW_BTN_R2 && chapter + 1 < book->nchapters) {
        open_chapter(chapter + 1, 0, false);
        remember_position();
    }
    else if (key == SW_BTN_L2) {
        // to the start of this chapter, or the previous one when already there
        open_chapter(page == 0 && chapter > 0 ? chapter - 1 : chapter, 0, false);
        remember_position();
    }
    else if (key == SW_BTN_START || key == SW_BTN_MENU) {
        overlay = OV_SETTINGS;
        ov_sel = 0;
    }
    else if (key == SW_BTN_SELECT) {
        overlay = OV_TOC;
        ov_sel = toc_index_for_chapter();
        ov_scroll = ov_sel > 3 ? ov_sel - 3 : 0;
    }
    else if (key == SW_BTN_B) {
        stop_reading();
        scr = detail ? S_DETAIL : S_SHELF;
    }
}

static Entry *latest_reading(void)
{
    Entry *best = NULL;
    for (int i = 0; i < nlib; i++)
        if (lib[i].last_read > 0 && !lib[i].finished && (!best || lib[i].last_read > best->last_read))
            best = &lib[i];
    return best;
}

static void on_key(SDLKey key)
{
    dirty = true;
    if (scr == S_READ) {
        on_key_reader(key);
        return;
    }
    if (scr == S_DETAIL) {
        if (key == SW_BTN_A)
            start_reading(detail, false);
        else if (key == SW_BTN_X)
            start_reading(detail, true);
        else if (key == SW_BTN_Y) {
            detail->finished = !detail->finished;
            if (detail->finished) {
                detail->progress = 1.0;
                if (!detail->last_read)
                    detail->last_read = (long)time(NULL);
            }
            else {
                detail->progress = 0;
                detail->chapter = detail->offset = 0;
                detail->last_read = 0;
            }
            mark_lib_dirty();
        }
        else if (key == SW_BTN_B) {
            scr = S_SHELF;
            detail = NULL;
            refresh_shelf();
        }
        return;
    }
    // shelf
    if (key == SW_BTN_LEFT && view_sel > 0)
        view_sel--;
    else if (key == SW_BTN_RIGHT && view_sel + 1 < nview)
        view_sel++;
    else if (key == SW_BTN_UP && view_sel >= 5)
        view_sel -= 5;
    else if (key == SW_BTN_DOWN && view_sel + 5 < nview)
        view_sel += 5;
    else if (key == SW_BTN_DOWN && view_sel / 5 < (nview - 1) / 5)
        view_sel = nview - 1;
    else if ((key == SW_BTN_L1 || key == SW_BTN_R1) && ntabs > 1) {
        tab_sel = (tab_sel + (key == SW_BTN_R1 ? 1 : ntabs - 1)) % ntabs;
        nview = 0;
        build_view();
    }
    else if (key == SW_BTN_X && tabs[tab_sel].kind != 1) {
        sort_mode = (sort_mode + 1) % SORT_COUNT;
        build_view();
        show_message(sort_names[sort_mode], 900);
        mark_lib_dirty();
    }
    else if (key == SW_BTN_A && nview)
        open_detail(&lib[view[view_sel]]);
    else if (key == SW_BTN_Y) {
        Entry *e = latest_reading();
        if (e) {
            detail = NULL;
            start_reading(e, false);
        }
    }
    else if (key == SW_BTN_B)
        quit = 1;
}

/* ---------- background work ---------- */

// Load one missing cover per call, visible ones first
static bool load_next_cover(void)
{
    if (scr == S_SHELF) {
        for (int k = view_scroll * 5; k < (view_scroll + 3) * 5 && k < nview; k++)
            if (!lib[view[k]].cover_state) {
                load_entry(&lib[view[k]]);
                return true;
            }
    }
    if (scr == S_DETAIL && detail && !detail->cover_state) {
        load_entry(detail);
        return true;
    }
    if (scr == S_READ)
        return false;
    for (int i = 0; i < nlib; i++)
        if (!lib[i].meta_done || !lib[i].cover_state) {
            load_entry(&lib[i]);
            return true;
        }
    return false;
}

/* ---------- scripted input for tests ---------- */

static char *script = NULL, *spos = NULL;
static Uint32 swait = 0;

static void script_step(void)
{
    if (!spos || SDL_GetTicks() < swait)
        return;
    while (*spos == ' ')
        spos++;
    if (!*spos) {
        quit = 1;
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
    } keys[] = {{"U", SW_BTN_UP}, {"D", SW_BTN_DOWN}, {"L", SW_BTN_LEFT}, {"R", SW_BTN_RIGHT}, {"A", SW_BTN_A}, {"B", SW_BTN_B}, {"X", SW_BTN_X}, {"Y", SW_BTN_Y}, {"L1", SW_BTN_L1}, {"R1", SW_BTN_R1}, {"L2", SW_BTN_L2}, {"R2", SW_BTN_R2}, {"START", SW_BTN_START}, {"SELECT", SW_BTN_SELECT}};
    if (!strncmp(tok, "shot:", 5)) {
        draw();
        SDL_SaveBMP(screen, tok + 5);
    }
    else if (!strncmp(tok, "wait:", 5))
        swait = SDL_GetTicks() + atoi(tok + 5);
    else if (!strcmp(tok, "covers")) {
        // finish all background work before going on
        while (load_next_cover())
            ;
    }
    else
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(tok, keys[i].name))
                on_key(keys[i].key);
}

/* ---------- main ---------- */

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

int main(int argc, char **argv)
{
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    const char *e;
    copy_str(sdcard, sizeof(sdcard), (e = getenv("SDCARD")) ? e : "/mnt/SDCARD");
    if ((e = getenv("BOOKS_DIR")))
        copy_str(books_dir, sizeof(books_dir), e);
    else
        snprintf(books_dir, sizeof(books_dir), "%s/Media/Books", sdcard);
    snprintf(state_dir, sizeof(state_dir), "%s/Saves/CurrentProfile/states/Bokhylle", sdcard);
    snprintf(cover_dir, sizeof(cover_dir), "%s/covers", state_dir);
    snprintf(lib_path, sizeof(lib_path), "%s/library.json", state_dir);
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", state_dir);
    if ((e = getenv("BOOK_UI_FONT")))
        copy_str(ui_font, sizeof(ui_font), e);
    else
        snprintf(ui_font, sizeof(ui_font), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    // reading fonts live next to the program
    char exe[400];
    copy_str(exe, sizeof(exe), argc > 0 ? argv[0] : ".");
    char *slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';
    else
        copy_str(exe, sizeof(exe), ".");
    snprintf(font_dir, sizeof(font_dir), "%s/fonts", exe);
    if ((e = getenv("BOOK_SCRIPT")))
        spos = script = strdup(e);
    mkdir_p(books_dir);
    mkdir_p(cover_dir);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(400, 120);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!soup_open_fonts(ui_font)) {
        fprintf(stderr, "bokhylle: cannot open font %s\n", ui_font);
        return 1;
    }
    draw_loading("Finding books …");
    load_settings();
    load_library();
    refresh_shelf();

    while (!quit) {
        SDL_Event ev;
        bool input = false;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = 1;
            else if (ev.type == SDL_KEYDOWN && !spos) {
                on_key(ev.key.keysym.sym);
                input = true;
            }
        }
        script_step();
        if (*message && SDL_GetTicks() >= message_until) {
            message[0] = '\0';
            dirty = true;
        }
        if (dirty)
            draw();
        if (!input && load_next_cover()) {
            dirty = true;
            continue;
        }
        if (lib_dirty && SDL_GetTicks() - lib_changed_at > 4000)
            save_library();
        SDL_Delay(dirty ? 5 : 30);
    }
    if (scr == S_READ)
        remember_position();
    save_library();
    free(script);
    TTF_Quit();
    SDL_Quit();
    return 0;
}

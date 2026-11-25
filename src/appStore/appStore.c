// OnionSoup App Store: graphical front end for .tmp_update/script/appstore.sh
//
// The shell script does all network and file work (fetch, install, remove,
// image) and reports progress in <workdir>/status. This program draws the
// store and runs the script in the background.
//
// Environment overrides for testing on a PC:
//   SDCARD              SD card root (default /mnt/SDCARD)
//   APPSTORE_SH         path to appstore.sh
//   APPSTORE_FONT       TTF font
//   APPSTORE_SCRIPT     scripted input, e.g. "R R D A shot:out.bmp wait:500"

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "system/keymap_sw.h"

#define SCREEN_W 640
#define SCREEN_H 480
#define TOP_H 52
#define BOTTOM_Y 446
#define MAX_ITEMS 512

#define CARD_W 100
#define CARD_H 140
#define SHELF_W 88
#define SHELF_H 124
#define HERO_W 128
#define HERO_H 180
#define DETAIL_W 180
#define DETAIL_H 253

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

typedef struct {
    char id[128];
    char name[160];
    char author[128];
    char description[512];
    char version[48];
    char category[64];
    char genre[64];
    char image[512];
    bool is_port;
    bool needs_files;
    long size;
    char installed[48]; // installed version, "" if not installed
    // image state: 0 = not requested, 1 = loading, 2 = loaded, 3 = failed
    int img_state;
    SDL_Surface *img_card, *img_shelf, *img_hero, *img_detail;
} Item;

typedef enum { TAB_FEATURED, TAB_APPS, TAB_PORTS, TAB_INSTALLED, TAB_COUNT } Tab;
static const char *TAB_NAMES[TAB_COUNT] = {"Utvalgt", "Apper", "Porter", "Installert"};

static Item items[MAX_ITEMS];
static int item_count = 0;

static char sdcard[256], workdir[300], script_path[300], font_path[300];
static SDL_Surface *video, *screen;
static TTF_Font *f13, *f15, *f17, *f19, *f24, *f34;

// UI state
static Tab tab = TAB_FEATURED;
static int sel = 0, scroll_row = 0;         // grid tabs
static int feat_row = 0, feat_col = 0;      // featured tab: row 0 = hero, 1 = free ports, 2 = apps
static int feat_scroll = 0;
static int detail = -1;                     // item index shown in detail view, -1 = none
static int hero = -1;

// background job (install/remove/fetch)
static pid_t job_pid = 0;
static char job_kind[16] = "";
static char job_status[256] = "";
static int job_item = -1;
static char message[256] = "";
static Uint32 message_until = 0;

// background image download
static pid_t img_pid = 0;
static int img_item = -1;

static bool quit = false;

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

static void show_message(const char *text, Uint32 ms)
{
    copy_str(message, sizeof(message), text);
    message_until = SDL_GetTicks() + ms;
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

static void load_item_images(Item *it)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/img/%s.png", workdir, it->id);
    SDL_Surface *img = IMG_Load(path);
    if (!img) {
        it->img_state = 3;
        return;
    }
    it->img_card = scale_rounded(img, CARD_W, CARD_H, 8);
    it->img_shelf = scale_rounded(img, SHELF_W, SHELF_H, 8);
    it->img_hero = scale_rounded(img, HERO_W, HERO_H, 10);
    it->img_detail = scale_rounded(img, DETAIL_W, DETAIL_H, 12);
    SDL_FreeSurface(img);
    it->img_state = 2;
}

static pid_t spawn(const char *cmd, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execl("/bin/sh", "sh", script_path, cmd, arg, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void request_image(int idx)
{
    if (idx < 0 || idx >= item_count)
        return;
    Item *it = &items[idx];
    if (it->img_state != 0)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/img/%s.png", workdir, it->id);
    if (file_exists(path)) {
        load_item_images(it);
        return;
    }
    if (!*it->image) {
        it->img_state = 3;
        return;
    }
    if (img_pid > 0)
        return; // one download at a time; retried next frame
    it->img_state = 1;
    img_item = idx;
    img_pid = spawn("image", it->id);
}

static void poll_image_job(void)
{
    if (img_pid <= 0)
        return;
    int status;
    if (waitpid(img_pid, &status, WNOHANG) == img_pid) {
        img_pid = 0;
        Item *it = &items[img_item];
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            load_item_images(it);
        else
            it->img_state = 3;
        img_item = -1;
    }
}

// Card picture or a placeholder with the first letter
static void draw_cover(Item *it, SDL_Surface *img, int x, int y, int w, int h)
{
    if (img) {
        SDL_Rect pos = {x, y};
        SDL_BlitSurface(img, NULL, screen, &pos);
        return;
    }
    fill_round(x, y, w, h, 8, C_CARD);
    char letter[2] = {(char)toupper((unsigned char)it->name[0]), 0};
    TTF_Font *f = w > 120 ? f34 : f24;
    text(f, letter, x + (w - text_w(f, letter)) / 2, y + h / 2 - TTF_FontHeight(f) / 2, C_ACCENT);
}

/* ---------- catalog ---------- */

static const char *json_str(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void refresh_installed(Item *it)
{
    char path[600];
    if (it->is_port)
        snprintf(path, sizeof(path), "%s/installed/%s.version", workdir, it->id);
    else
        snprintf(path, sizeof(path), "%s/App/%s/.appstore_version", sdcard, it->id);
    read_file_line(path, it->installed, sizeof(it->installed));
}

static int cmp_items(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->is_port != y->is_port)
        return x->is_port - y->is_port; // apps first
    if (x->needs_files != y->needs_files)
        return x->needs_files - y->needs_files; // free ports first
    return strcasecmp(x->name, y->name);
}

static bool load_catalog(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/catalog.json", workdir);
    char *data = read_all(path);
    if (!data)
        return false;
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root)
        return false;

    char model[16];
    read_file_line("/tmp/deviceModel", model, sizeof(model));

    item_count = 0;
    cJSON *apps = cJSON_GetObjectItem(root, "apps"), *a;
    cJSON_ArrayForEach(a, apps)
    {
        if (item_count >= MAX_ITEMS)
            break;
        cJSON *devices = cJSON_GetObjectItem(a, "devices");
        if (*model && cJSON_IsArray(devices)) {
            bool ok = false;
            cJSON *d;
            cJSON_ArrayForEach(d, devices) if (cJSON_IsString(d) && !strcmp(d->valuestring, model)) ok = true;
            if (!ok)
                continue;
        }
        Item *it = &items[item_count++];
        memset(it, 0, sizeof(*it));
        copy_str(it->id, sizeof(it->id), json_str(a, "id"));
        copy_str(it->name, sizeof(it->name), json_str(a, "name"));
        copy_str(it->author, sizeof(it->author), json_str(a, "author"));
        copy_str(it->description, sizeof(it->description), json_str(a, "description"));
        copy_str(it->version, sizeof(it->version), json_str(a, "version"));
        copy_str(it->category, sizeof(it->category), json_str(a, "category"));
        copy_str(it->genre, sizeof(it->genre), json_str(a, "genre"));
        copy_str(it->image, sizeof(it->image), json_str(a, "image"));
        it->is_port = !strcmp(json_str(a, "type"), "port");
        it->needs_files = cJSON_IsTrue(cJSON_GetObjectItem(a, "requires_game_files"));
        cJSON *size = cJSON_GetObjectItem(a, "size");
        it->size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
        refresh_installed(it);
    }
    cJSON_Delete(root);
    qsort(items, item_count, sizeof(Item), cmp_items);

    // Featured item: a free port that changes every day
    int free_ports[MAX_ITEMS], n = 0;
    for (int i = 0; i < item_count; i++)
        if (items[i].is_port && !items[i].needs_files)
            free_ports[n++] = i;
    if (n) {
        time_t now = time(NULL);
        hero = free_ports[(now / 86400) % n];
    }
    else {
        hero = item_count ? 0 : -1;
    }
    return item_count > 0;
}

/* ---------- lists per tab ---------- */

static int tab_list(Tab t, int *out)
{
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        bool ok = (t == TAB_APPS && !it->is_port) || (t == TAB_PORTS && it->is_port) ||
                  (t == TAB_INSTALLED && *it->installed);
        if (ok)
            out[n++] = i;
    }
    return n;
}

static int shelf_list(int row, int *out)
{
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        if (row == 1 && it->is_port && !it->needs_files && i != hero)
            out[n++] = i;
        if (row == 2 && (!it->is_port || it->needs_files))
            out[n++] = i;
    }
    return n;
}

/* ---------- jobs ---------- */

static void start_job(const char *kind, int idx)
{
    if (job_pid > 0)
        return;
    copy_str(job_kind, sizeof(job_kind), kind);
    job_item = idx;
    char status_path[400];
    snprintf(status_path, sizeof(status_path), "%s/status", workdir);
    remove(status_path);
    job_status[0] = '\0';
    job_pid = spawn(kind, idx >= 0 ? items[idx].id : NULL);
}

// Returns true when the job just finished
static bool poll_job(int *exit_code)
{
    if (job_pid <= 0)
        return false;
    char status_path[400];
    snprintf(status_path, sizeof(status_path), "%s/status", workdir);
    read_file_line(status_path, job_status, sizeof(job_status));
    int status;
    if (waitpid(job_pid, &status, WNOHANG) == job_pid) {
        job_pid = 0;
        read_file_line(status_path, job_status, sizeof(job_status));
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        return true;
    }
    return false;
}

// "<state> <percent> <text>"
static void parse_status(char *state, int *pct, char *txt, size_t txt_n)
{
    char st[32] = "";
    int p = 0, off = 0;
    *txt = '\0';
    if (sscanf(job_status, "%31s %d %n", st, &p, &off) >= 2)
        copy_str(txt, txt_n, job_status + off);
    copy_str(state, 32, st);
    *pct = p < 0 ? 0 : (p > 100 ? 100 : p);
}

/* ---------- screens ---------- */

static void draw_topbar(void)
{
    fill(0, 0, SCREEN_W, TOP_H, C_BAR);
    text(f24, "onion", 18, 10, C_ACCENT);
    int x = 18 + text_w(f24, "onion");
    text(f24, "soup", x, 10, C_WHITE);
    x += text_w(f24, "soup") + 8;
    text(f17, "App Store", x, 16, C_GREY);

    int tx = SCREEN_W - 16;
    for (int i = TAB_COUNT - 1; i >= 0; i--)
        tx -= text_w(f17, TAB_NAMES[i]) + (i ? 26 : 0);
    for (int i = 0; i < TAB_COUNT; i++) {
        int w = text_w(f17, TAB_NAMES[i]);
        if (i == (int)tab && detail < 0) {
            fill_round(tx - 10, 12, w + 20, 28, 14, C_ACCENT);
            text(f17, TAB_NAMES[i], tx, 15, C_DARK);
        }
        else if (i == (int)tab) {
            fill_round(tx - 10, 12, w + 20, 28, 14, C_PANEL);
            text(f17, TAB_NAMES[i], tx, 15, C_ACCENT);
        }
        else {
            text(f17, TAB_NAMES[i], tx, 15, C_GREY);
        }
        tx += w + 26;
    }
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

static void draw_badge(Item *it, int x, int y, int w)
{
    if (!*it->installed)
        return;
    bool update = strcmp(it->installed, it->version) != 0;
    const char *label = update ? "NY" : "OK";
    int bw = text_w(f13, label) + 12;
    fill_round(x + w - bw - 5, y + 5, bw, 18, 9, update ? C_ACCENT : C_GREEN);
    text(f13, label, x + w - bw + 1, y + 6, update ? C_DARK : C_WHITE);
}

static void draw_card(int idx, int x, int y, int w, int h, bool selected, bool shelf)
{
    Item *it = &items[idx];
    request_image(idx);
    if (selected)
        outline_round(x - 5, y - 5, w + 10, h + 10, 12, 3, C_ACCENT, C_BG);
    draw_cover(it, shelf ? it->img_shelf : it->img_card, x, y, w, h);
    draw_badge(it, x, y, w);
    char name[160];
    copy_str(name, sizeof(name), it->name);
    char *paren = strstr(name, " (");
    if (paren)
        *paren = '\0';
    text_fit(f13, name, x, y + h + 6, w, selected ? C_WHITE : C_GREY);
}

static void draw_featured(void)
{
    int y0 = TOP_H - feat_scroll;
    // hero
    if (hero >= 0) {
        Item *it = &items[hero];
        request_image(hero);
        fill(0, y0, SCREEN_W, 200, C_PANEL);
        if (it->img_detail) {
            // soft banner: stretched, darkened cover
            static SDL_Surface *band = NULL, *shade = NULL;
            static int band_for = -1;
            if (band_for != hero) {
                if (band)
                    SDL_FreeSurface(band);
                band = scale_rounded(it->img_detail, SCREEN_W, 200, 0);
                band_for = hero;
            }
            if (!shade) {
                shade = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, 200, 32, 0, 0, 0, 0);
                SDL_FillRect(shade, NULL, SDL_MapRGB(shade->format, 22, 18, 16));
                SDL_SetAlpha(shade, SDL_SRCALPHA, 205);
            }
            if (band) {
                SDL_Rect pos = {0, y0};
                SDL_BlitSurface(band, NULL, screen, &pos);
                pos.x = 0;
                pos.y = y0;
                SDL_BlitSurface(shade, NULL, screen, &pos);
            }
        }
        draw_cover(it, it->img_hero, 24, y0 + 10, HERO_W, HERO_H);
        text(f13, "UTVALGT", 172, y0 + 22, C_ACCENT);
        char name[160];
        copy_str(name, sizeof(name), it->name);
        char *paren = strstr(name, " (");
        if (paren)
            *paren = '\0';
        text_fit(f34, name, 172, y0 + 38, SCREEN_W - 190, C_WHITE);
        text_wrap(f15, it->description, 172, y0 + 84, SCREEN_W - 196, 20, 2, C_SOFT);
        char meta[128], sz[32];
        human_size(it->size, sz, sizeof(sz));
        snprintf(meta, sizeof(meta), "%s  ·  %s", *it->genre ? it->genre : "Porter", sz);
        text_fit(f15, meta, 172, y0 + 126, SCREEN_W - 196, C_GREY);
        const char *btn = *it->installed ? "Installert" : "Installer";
        int bw = text_w(f19, btn) + 40;
        if (feat_row == 0)
            outline_round(168, y0 + 150, bw + 8, 44, 22, 3, C_WHITE, C_BG);
        fill_round(172, y0 + 154, bw, 36, 18, *it->installed ? C_GREEN : C_ACCENT);
        text(f19, btn, 192, y0 + 160, *it->installed ? C_WHITE : C_DARK);
    }

    // shelves
    static const char *titles[] = {"", "Gratis porter", "Apper og motorer"};
    int y = y0 + 210;
    for (int row = 1; row <= 2; row++) {
        int list[MAX_ITEMS];
        int n = shelf_list(row, list);
        text(f19, titles[row], 24, y, C_WHITE);
        char count[16];
        snprintf(count, sizeof(count), "%d", n);
        text(f15, count, SCREEN_W - 24 - text_w(f15, count), y + 4, C_GREY);
        int first = 0;
        if (feat_row == row && feat_col > 4)
            first = feat_col - 4;
        for (int k = 0; k < 6 && first + k < n; k++) {
            int i = first + k;
            int x = 24 + k * (SHELF_W + 14);
            draw_card(list[i], x, y + 32, SHELF_W, SHELF_H, feat_row == row && feat_col == i, true);
        }
        y += 190;
    }
}

static void draw_grid(void)
{
    int list[MAX_ITEMS];
    int n = tab_list(tab, list);
    const char *title = tab == TAB_APPS ? "Apper" : tab == TAB_PORTS ? "Porter" : "Installert";
    text(f19, title, 24, TOP_H + 8, C_WHITE);
    char count[32];
    snprintf(count, sizeof(count), "%d", n);
    text(f15, count, 24 + text_w(f19, title) + 10, TOP_H + 12, C_GREY);
    if (n == 0) {
        const char *empty = tab == TAB_INSTALLED ? "Ingenting installert fra App Store ennå." : "Ingenting her ennå.";
        text(f17, empty, (SCREEN_W - text_w(f17, empty)) / 2, 230, C_GREY);
        return;
    }
    if (sel >= n)
        sel = n - 1;
    int row = sel / 5;
    if (row < scroll_row)
        scroll_row = row;
    if (row > scroll_row + 1)
        scroll_row = row - 1;
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 5; c++) {
            int i = (scroll_row + r) * 5 + c;
            if (i >= n)
                break;
            int x = 24 + c * 120;
            int y = TOP_H + 42 + r * 176;
            draw_card(list[i], x, y, CARD_W, CARD_H, i == sel, false);
        }
    }
    // scroll hint
    int rows = (n + 4) / 5;
    if (rows > 2) {
        int bar_h = 330 * 2 / rows;
        int bar_y = TOP_H + 44 + (330 - bar_h) * scroll_row / (rows - 2);
        fill_round(SCREEN_W - 10, TOP_H + 44, 4, 330, 2, C_PANEL);
        fill_round(SCREEN_W - 10, bar_y, 4, bar_h, 2, C_ACCENT);
    }
}

static void draw_detail(void)
{
    Item *it = &items[detail];
    request_image(detail);
    draw_cover(it, it->img_detail, 24, TOP_H + 18, DETAIL_W, DETAIL_H);

    int x = 228, w = SCREEN_W - x - 24;
    char name[160];
    copy_str(name, sizeof(name), it->name);
    char *paren = strstr(name, " (");
    char engine[64] = "";
    if (paren) {
        copy_str(engine, sizeof(engine), paren + 2);
        engine[strcspn(engine, ")")] = '\0';
        *paren = '\0';
    }
    int y = text_wrap(f34, name, x, TOP_H + 14, w, 38, 2, C_WHITE);
    char by[200];
    if (*engine)
        snprintf(by, sizeof(by), "%s  ·  %s", it->author, engine);
    else
        copy_str(by, sizeof(by), it->author);
    text_fit(f15, by, x, y + 2, w, C_GREY);
    y += 30;

    // chips
    char sz[32];
    human_size(it->size, sz, sizeof(sz));
    const char *chips[3] = {it->is_port ? (it->needs_files ? "Egne spillfiler" : "Gratis") : "App",
                            *it->genre ? it->genre : NULL, sz};
    int cx = x;
    for (int i = 0; i < 3; i++) {
        if (!chips[i])
            continue;
        char chip[64];
        copy_str(chip, sizeof(chip), chips[i]);
        int cw = text_w(f13, chip) + 22;
        if (cx + cw > SCREEN_W - 24)
            break;
        fill_round(cx, y, cw, 24, 12, C_CARD);
        text(f13, chip, cx + 11, y + 3, i == 0 && it->needs_files ? C_ACCENT : C_WHITE);
        cx += cw + 8;
    }
    y += 40;
    y = text_wrap(f15, it->description, x, y, w, 21, 5, C_SOFT);

    // action area
    int by_ = TOP_H + 222;
    char state[32], txt[128];
    int pct = 0;
    if (job_pid > 0 && job_item == detail) {
        parse_status(state, &pct, txt, sizeof(txt));
        fill_round(x, by_, w, 36, 18, C_PANEL);
        int pw = w * pct / 100;
        if (!strcmp(state, "download") && pw > 36)
            fill_round(x, by_, pw, 36, 18, C_ACCENT);
        char line[160];
        if (!strcmp(state, "download"))
            snprintf(line, sizeof(line), "Laster ned … %d %%", pct);
        else if (!strcmp(state, "verify"))
            snprintf(line, sizeof(line), "Sjekker filen …");
        else if (!strcmp(job_kind, "remove"))
            snprintf(line, sizeof(line), "Fjerner …");
        else
            snprintf(line, sizeof(line), "Installerer …");
        text(f17, line, x + 14, by_ + 7, !strcmp(state, "download") && pw > 120 ? C_DARK : C_WHITE);
    }
    else if (*it->installed) {
        bool update = strcmp(it->installed, it->version) != 0;
        const char *label = update ? "Oppdater" : "Installert";
        fill_round(x, by_, text_w(f19, label) + 40, 36, 18, update ? C_ACCENT : C_GREEN);
        text(f19, label, x + 20, by_ + 6, update ? C_DARK : C_WHITE);
    }
    else {
        fill_round(x, by_, text_w(f19, "Installer") + 40, 36, 18, C_ACCENT);
        text(f19, "Installer", x + 20, by_ + 6, C_DARK);
    }

    char foot[200];
    snprintf(foot, sizeof(foot), "Versjon %s%s%s", it->version, *it->installed ? "  ·  installert " : "",
             *it->installed ? it->installed : "");
    text_fit(f13, foot, 24, TOP_H + 290, SCREEN_W - 48, C_GREY);
    if (it->is_port)
        text_fit(f13,
                 it->needs_files ? "Legg inn originale spillfiler, se _required_files.txt i Roms/PORTS/Games."
                                 : "Krever «Ports Collection» i Package Manager. Kjør «~Import ports» etterpå.",
                 24, TOP_H + 312, SCREEN_W - 48, C_GREY);
}

static void draw_message(void)
{
    if (!*message || SDL_GetTicks() > message_until)
        return;
    int w = text_w(f17, message) + 40;
    if (w > SCREEN_W - 40)
        w = SCREEN_W - 40;
    int x = (SCREEN_W - w) / 2;
    fill_round(x, BOTTOM_Y - 56, w, 40, 20, C_PANEL);
    text_fit(f17, message, x + 20, BOTTOM_Y - 48, w - 40, C_WHITE);
}

static void draw(void)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    if (detail >= 0)
        draw_detail();
    else if (tab == TAB_FEATURED)
        draw_featured();
    else
        draw_grid();
    draw_topbar();

    if (detail >= 0) {
        Item *it = &items[detail];
        if (job_pid > 0) {
            const char *k[] = {"B"};
            const char *l[] = {"Tilbake"};
            draw_hints(k, l, 1);
        }
        else if (*it->installed) {
            bool update = strcmp(it->installed, it->version) != 0;
            const char *k[] = {"A", "Y", "B"};
            const char *l[] = {update ? "Oppdater" : "Installer på nytt", "Fjern", "Tilbake"};
            draw_hints(k, l, 3);
        }
        else {
            const char *k[] = {"A", "B"};
            const char *l[] = {"Installer", "Tilbake"};
            draw_hints(k, l, 2);
        }
    }
    else {
        const char *k[] = {"A", "B", "L/R"};
        const char *l[] = {"Åpne", "Avslutt", "Bytt fane"};
        draw_hints(k, l, 3);
    }
    draw_message();
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

static void draw_center(const char *line1, const char *line2, SDL_Color c1)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    draw_topbar();
    text(f24, line1, (SCREEN_W - text_w(f24, line1)) / 2, 200, c1);
    if (line2)
        text_wrap(f15, line2, 80, 240, SCREEN_W - 160, 20, 4, C_GREY);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

/* ---------- input ---------- */

static void open_detail(int idx)
{
    detail = idx;
}

static void on_key(SDLKey key)
{
    if (detail >= 0) {
        Item *it = &items[detail];
        if (key == SW_BTN_B) {
            detail = -1;
        }
        else if (key == SW_BTN_A && job_pid <= 0) {
            start_job("install", detail);
        }
        else if (key == SW_BTN_Y && job_pid <= 0 && *it->installed) {
            start_job("remove", detail);
        }
        return;
    }

    if (key == SW_BTN_B || key == SW_BTN_MENU) {
        quit = true;
        return;
    }
    if (key == SW_BTN_L1 || key == SW_BTN_R1) {
        tab = (tab + (key == SW_BTN_R1 ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
        sel = scroll_row = 0;
        return;
    }

    if (tab == TAB_FEATURED) {
        int list[MAX_ITEMS];
        int n = feat_row > 0 ? shelf_list(feat_row, list) : 1;
        if (key == SW_BTN_DOWN && feat_row < 2) {
            feat_row++;
            feat_col = 0;
        }
        else if (key == SW_BTN_UP && feat_row > 0) {
            feat_row--;
            feat_col = 0;
        }
        else if (key == SW_BTN_RIGHT && feat_col < n - 1)
            feat_col++;
        else if (key == SW_BTN_LEFT && feat_col > 0)
            feat_col--;
        else if (key == SW_BTN_A) {
            if (feat_row == 0 && hero >= 0)
                open_detail(hero);
            else if (feat_row > 0 && n > 0)
                open_detail(list[feat_col]);
        }
        feat_scroll = feat_row == 2 ? 190 : 0;
        return;
    }

    int list[MAX_ITEMS];
    int n = tab_list(tab, list);
    if (!n)
        return;
    if (key == SW_BTN_RIGHT && sel < n - 1)
        sel++;
    else if (key == SW_BTN_LEFT && sel > 0)
        sel--;
    else if (key == SW_BTN_DOWN)
        sel = sel + 5 < n ? sel + 5 : n - 1;
    else if (key == SW_BTN_UP && sel >= 5)
        sel -= 5;
    else if (key == SW_BTN_A)
        open_detail(list[sel]);
}

/* ---------- scripted input for tests ---------- */

static char *script_tokens = NULL;
static char *script_pos = NULL;
static Uint32 script_wait_until = 0;

static void save_shot(const char *path)
{
    SDL_SaveBMP(screen, path);
}

// Returns true while the script is running
static bool run_script_step(void)
{
    if (!script_pos)
        return false;
    if (SDL_GetTicks() < script_wait_until)
        return true;
    while (*script_pos == ' ')
        script_pos++;
    if (!strncmp(script_pos, "now:", 4)) { // screenshot while a job is running
        char shot[256];
        int k = 0;
        script_pos += 4;
        while (*script_pos && *script_pos != ' ' && k < 255)
            shot[k++] = *script_pos++;
        shot[k] = '\0';
        draw();
        save_shot(shot);
        return true;
    }
    if (job_pid > 0 || img_pid > 0)
        return true; // let background work finish first
    while (*script_pos == ' ')
        script_pos++;
    if (!*script_pos) {
        quit = true;
        script_pos = NULL;
        return false;
    }
    char tok[256];
    int n = 0;
    while (*script_pos && *script_pos != ' ' && n < 255)
        tok[n++] = *script_pos++;
    tok[n] = '\0';
    static const struct {
        const char *name;
        SDLKey key;
    } keys[] = {{"U", SW_BTN_UP}, {"D", SW_BTN_DOWN}, {"L", SW_BTN_LEFT}, {"R", SW_BTN_RIGHT},
                {"A", SW_BTN_A}, {"B", SW_BTN_B}, {"Y", SW_BTN_Y}, {"L1", SW_BTN_L1}, {"R1", SW_BTN_R1}};
    if (!strncmp(tok, "shot:", 5)) {
        draw();
        save_shot(tok + 5);
    }
    else if (!strncmp(tok, "wait:", 5)) {
        script_wait_until = SDL_GetTicks() + atoi(tok + 5);
    }
    else {
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(tok, keys[i].name))
                on_key(keys[i].key);
    }
    return true;
}

/* ---------- main ---------- */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *env;
    copy_str(sdcard, sizeof(sdcard), (env = getenv("SDCARD")) ? env : "/mnt/SDCARD");
    snprintf(workdir, sizeof(workdir), "%s/.tmp_update/appstore", sdcard);
    if ((env = getenv("APPSTORE_SH")))
        copy_str(script_path, sizeof(script_path), env);
    else
        snprintf(script_path, sizeof(script_path), "%s/.tmp_update/script/appstore.sh", sdcard);
    if ((env = getenv("APPSTORE_FONT")))
        copy_str(font_path, sizeof(font_path), env);
    else
        snprintf(font_path, sizeof(font_path), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    if ((env = getenv("APPSTORE_SCRIPT"))) {
        script_tokens = strdup(env);
        script_pos = script_tokens;
    }
    mkdir(workdir, 0755);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 60);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);

    f13 = TTF_OpenFont(font_path, 13);
    f15 = TTF_OpenFont(font_path, 15);
    f17 = TTF_OpenFont(font_path, 17);
    f19 = TTF_OpenFont(font_path, 19);
    f24 = TTF_OpenFont(font_path, 24);
    f34 = TTF_OpenFont(font_path, 34);
    if (!f13 || !f15 || !f17 || !f19 || !f24 || !f34) {
        fprintf(stderr, "appStore: cannot open font %s\n", font_path);
        return 1;
    }

    // Connect and download the catalog
    start_job("fetch", -1);
    int code = 0;
    int dots = 0;
    while (!poll_job(&code)) {
        char line[32];
        snprintf(line, sizeof(line), "Kobler til%.*s", (dots++ / 4) % 4, "...");
        draw_center(line, NULL, C_WHITE);
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SW_BTN_B || e.key.keysym.sym == SW_BTN_MENU)) {
                kill(job_pid, SIGTERM);
                goto done;
            }
        SDL_Delay(100);
    }

    if (!load_catalog()) {
        char state[32], txt[200];
        int pct;
        parse_status(state, &pct, txt, sizeof(txt));
        draw_center("Kunne ikke laste App Store", *txt ? txt : "Sjekk WiFi-tilkoblingen og prøv igjen.", C_RED);
        if (script_pos) {
            while (run_script_step())
                SDL_Delay(10);
            goto done;
        }
        SDL_Event e;
        for (;;) {
            if (SDL_WaitEvent(&e) && e.type == SDL_KEYDOWN)
                break;
        }
        goto done;
    }

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = true;
            else if (e.type == SDL_KEYDOWN && !script_pos)
                on_key(e.key.keysym.sym);
        }
        run_script_step();

        if (poll_job(&code)) {
            if (job_item >= 0) {
                Item *it = &items[job_item];
                refresh_installed(it);
                char state[32], txt[200];
                int pct;
                parse_status(state, &pct, txt, sizeof(txt));
                if (code == 0)
                    show_message(!strcmp(job_kind, "remove") ? "Fjernet" : "Installert!", 2500);
                else
                    show_message(*txt ? txt : "Noe gikk galt", 4000);
            }
            job_item = -1;
        }
        poll_image_job();
        draw();
        SDL_Delay(30);
    }

done:
    if (job_pid > 0)
        waitpid(job_pid, NULL, 0);
    if (img_pid > 0)
        waitpid(img_pid, NULL, 0);
    free(script_tokens);
    TTF_Quit();
    SDL_Quit();
    return 0;
}

// OnionSoup App Store: graphical front end for .tmp_update/script/appstore.sh
//
// The shell script does all network and file work (fetch, install, remove,
// apply, image) and reports progress in <workdir>/status. This program draws
// the store and runs the script in the background.
//
// Long lists scroll up and down only: the small screen makes sideways
// scrolling hard to follow. Shelves on the front page show a few items and a
// "Se alle" tile that opens the full list.
//
// Environment overrides for testing on a PC:
//   SDCARD              SD card root (default /mnt/SDCARD)
//   APPSTORE_SH         path to appstore.sh
//   APPSTORE_FONT       TTF font
//   APPSTORE_SCRIPT     scripted input, e.g. "R1 R1 D A shot:out.bmp wait:500"

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
#include "soup/soup_ui.h"
#include "system/keymap_sw.h"

#define MAX_ITEMS 1024
#define MAX_VARIANTS 12
#define MAX_LOADED 40 // items with scaled pictures kept in memory

// Portrait covers (apps and ports)
#define CARD_W 88
#define CARD_H 124
#define ROW_W 36
#define ROW_H 50
#define HERO_W 128
#define HERO_H 180
#define DETAIL_W 180
#define DETAIL_H 253
// Landscape previews (themes, 4:3 like the screen)
#define TCARD_W 128
#define TCARD_H 96
#define TTILE_W 180
#define TTILE_H 135
#define TROW_W 56
#define TROW_H 42
#define TDETAIL_W 256
#define TDETAIL_H 192

#define LIST_Y (TOP_H + 44)
#define LIST_ROW_H 58
#define LIST_ROWS 6
#define TGRID_COLS 3
#define TGRID_ROWS 2
#define TGRID_ROW_H 172

typedef enum { KIND_APP,
               KIND_PORT,
               KIND_THEME,
               KIND_GAME } Kind; // KIND_GAME = homebrew ROM (Game Store)

typedef enum { IMG_CARD,
               IMG_TILE,
               IMG_ROW,
               IMG_HERO,
               IMG_DETAIL,
               IMG_KINDS } ImgKind;

typedef struct {
    char id[128];
    char name[160];
    char author[128];
    char description[512];
    char version[48];
    char genre[64];
    char image[512];
    char system[16]; // Emu/ folder the game needs (Game Store), e.g. "GBA" or "PORTS"
    Kind kind;
    bool needs_files;
    int featured; // rank in the theme list's "featured" order, 0 = not featured
    long size;
    char variants[MAX_VARIANTS][128]; // theme folders inside a theme pack
    int variant_count;
    char installed[48]; // installed version, "" if not installed
    // image state: 0 = not requested, 1 = downloading, 2 = file ready, 3 = failed
    int img_state;
    SDL_Surface *img[IMG_KINDS];
    Uint32 img_used;
} Item;

typedef enum { TAB_FEATURED,
               TAB_APPS,
               TAB_PORTS,
               TAB_THEMES,
               TAB_INSTALLED,
               TAB_COUNT } Tab;
static const char *TAB_NAMES[TAB_COUNT] = {"Utvalgt", "Apper", "Porter", "Temaer", "Installert"};

// Game Store mode (--games): ports and homebrew games, filtered by the
// systems this device has emulators for. The App Store keeps apps and themes.
static bool games = false;
static const Tab APP_TABS[] = {TAB_FEATURED, TAB_APPS, TAB_THEMES, TAB_INSTALLED};
static const Tab GAME_TABS[] = {TAB_FEATURED, TAB_PORTS, TAB_APPS, TAB_INSTALLED};
#define TAB_N 4
#define MAX_SYSTEMS 48
static char systems[MAX_SYSTEMS][16];
static int system_count = 0;
static int sys_filter = -1; // index in systems[], -1 = all

static const Tab *tabs(void) { return games ? GAME_TABS : APP_TABS; }

static const char *tab_name(Tab t)
{
    return games && t == TAB_APPS ? "Homebrew" : TAB_NAMES[t];
}

static bool passes_filter(const Item *it)
{
    return !games || sys_filter < 0 || !strcmp(it->system, systems[sys_filter]);
}

// Rows on the front page
typedef enum { ROW_HERO,
               ROW_PORTS,
               ROW_THEMES,
               ROW_APPS } RowKind;

static Item items[MAX_ITEMS];
static int item_count = 0;

static char sdcard[256], workdir[300], script_path[300], font_path[300];
static char active_theme[256] = "";

// UI state
static Tab tab = TAB_FEATURED;
static int sel[TAB_COUNT], top[TAB_COUNT]; // selection and first visible row per tab
static RowKind feat_rows[4];
static int feat_row_count = 0;
static int feat_row = 0, feat_col = 0;
static int detail = -1; // item index shown in detail view, -1 = none
static int variant = 0; // selected theme in a theme pack (detail view)
static int hero = -1;
static Uint32 frame = 0;

// background job (install/remove/apply/fetch)
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

static void show_message(const char *text, Uint32 ms)
{
    copy_str(message, sizeof(message), text);
    message_until = SDL_GetTicks() + ms;
}

// Name without " (engine)" or " by author"
static void short_name(const char *name, char *out, size_t n)
{
    copy_str(out, n, name);
    char *cut = strstr(out, " (");
    if (cut)
        *cut = '\0';
    cut = strstr(out, " by ");
    if (cut)
        *cut = '\0';
}

static bool is_theme(const Item *it)
{
    return it->kind == KIND_THEME;
}

static bool has_update(const Item *it)
{
    return *it->installed && strcmp(it->installed, it->version) != 0;
}

static void read_active_theme(void)
{
    char path[400], line[300];
    snprintf(path, sizeof(path), "%s/.tmp_update/config/active_theme", sdcard);
    read_file_line(path, line, sizeof(line));
    int len = strlen(line);
    while (len > 0 && line[len - 1] == '/')
        line[--len] = '\0';
    const char *base = strrchr(line, '/');
    copy_str(active_theme, sizeof(active_theme), base ? base + 1 : line);
}

// Index of the variant in use, or -1 if this theme is not the active one
static int active_variant(const Item *it)
{
    if (!is_theme(it) || !*it->installed || !*active_theme)
        return -1;
    for (int i = 0; i < it->variant_count; i++)
        if (!strcmp(it->variants[i], active_theme))
            return i;
    return -1;
}

/* ---------- images ---------- */

static void img_size(const Item *it, ImgKind k, int *w, int *h)
{
    static const int portrait[IMG_KINDS][2] = {{CARD_W, CARD_H}, {CARD_W, CARD_H}, {ROW_W, ROW_H}, {HERO_W, HERO_H}, {DETAIL_W, DETAIL_H}};
    static const int landscape[IMG_KINDS][2] = {{TCARD_W, TCARD_H}, {TTILE_W, TTILE_H}, {TROW_W, TROW_H}, {TCARD_W, TCARD_H}, {TDETAIL_W, TDETAIL_H}};
    const int (*t)[2] = is_theme(it) ? landscape : portrait;
    *w = t[k][0];
    *h = t[k][1];
}

static void free_item_images(Item *it)
{
    for (int k = 0; k < IMG_KINDS; k++) {
        if (it->img[k])
            SDL_FreeSurface(it->img[k]);
        it->img[k] = NULL;
    }
}

// Keep memory use bounded: drop the pictures of the least recently drawn items
static void evict_images(void)
{
    int loaded = 0, oldest = -1;
    for (int i = 0; i < item_count; i++) {
        bool any = false;
        for (int k = 0; k < IMG_KINDS; k++)
            any = any || items[i].img[k];
        if (!any)
            continue;
        loaded++;
        if (items[i].img_used != frame && (oldest < 0 || items[i].img_used < items[oldest].img_used))
            oldest = i;
    }
    if (loaded > MAX_LOADED && oldest >= 0)
        free_item_images(&items[oldest]);
}

static pid_t spawn(const char *cmd, const char *arg, const char *arg2)
{
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execl("/bin/sh", "sh", script_path, cmd, arg, arg2, (char *)NULL);
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
        it->img_state = 2;
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
    img_pid = spawn("image", it->id, NULL);
}

static void poll_image_job(void)
{
    if (img_pid <= 0)
        return;
    int status;
    if (waitpid(img_pid, &status, WNOHANG) == img_pid) {
        img_pid = 0;
        items[img_item].img_state = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 2 : 3;
        img_item = -1;
    }
}

// Scaled picture of an item, or NULL while it is not available
static SDL_Surface *item_img(int idx, ImgKind k)
{
    Item *it = &items[idx];
    request_image(idx);
    it->img_used = frame;
    if (it->img[k] || it->img_state != 2)
        return it->img[k];
    char path[512];
    snprintf(path, sizeof(path), "%s/img/%s.png", workdir, it->id);
    SDL_Surface *src = IMG_Load(path);
    if (!src) {
        it->img_state = 3;
        return NULL;
    }
    int w, h;
    img_size(it, k, &w, &h);
    it->img[k] = scale_rounded(src, w, h, k == IMG_ROW ? 5 : (k == IMG_DETAIL ? 12 : 8));
    SDL_FreeSurface(src);
    evict_images();
    return it->img[k];
}

// Picture or a placeholder with the first letter
static void draw_cover(int idx, ImgKind k, int x, int y)
{
    Item *it = &items[idx];
    int w, h;
    img_size(it, k, &w, &h);
    SDL_Surface *img = item_img(idx, k);
    if (img) {
        SDL_Rect pos = {x, y};
        SDL_BlitSurface(img, NULL, screen, &pos);
        return;
    }
    fill_round(x, y, w, h, k == IMG_ROW ? 5 : 8, C_CARD);
    char letter[2] = {(char)toupper((unsigned char)it->name[0]), 0};
    TTF_Font *f = w > 120 ? f34 : (w > 60 ? f24 : f17);
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
    if (it->kind == KIND_APP)
        snprintf(path, sizeof(path), "%s/App/%s/.appstore_version", sdcard, it->id);
    else
        snprintf(path, sizeof(path), "%s/installed/%s.version", workdir, it->id);
    read_file_line(path, it->installed, sizeof(it->installed));
}

static int cmp_items(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->kind != y->kind)
        return x->kind - y->kind; // apps, ports, themes
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
        const char *type = json_str(a, "type");
        bool hack = !strcmp(type, "romhack"); // patch for the user's own ROM
        bool game_item = !strcmp(type, "port") || !strcmp(type, "game") || hack;
        if (game_item != games)
            continue;
        const char *sys = !strcmp(type, "port") ? "PORTS" : json_str(a, "system");
        if (games) {
            char emu[400];
            snprintf(emu, sizeof(emu), "%s/Emu/%s", sdcard, sys);
            struct stat st;
            if (!*sys || strchr(sys, '/') || stat(emu, &st) != 0 || !S_ISDIR(st.st_mode))
                continue; // no emulator for this system on the device
        }
        Item *it = &items[item_count++];
        memset(it, 0, sizeof(*it));
        copy_str(it->system, sizeof(it->system), sys);
        copy_str(it->id, sizeof(it->id), json_str(a, "id"));
        copy_str(it->name, sizeof(it->name), json_str(a, "name"));
        copy_str(it->author, sizeof(it->author), json_str(a, "author"));
        copy_str(it->description, sizeof(it->description), json_str(a, "description"));
        copy_str(it->version, sizeof(it->version), json_str(a, "version"));
        copy_str(it->genre, sizeof(it->genre), json_str(a, "genre"));
        copy_str(it->image, sizeof(it->image), json_str(a, "image"));
        it->kind = !strcmp(type, "port") ? KIND_PORT : (!strcmp(type, "game") || hack ? KIND_GAME : (!strcmp(type, "theme") ? KIND_THEME : KIND_APP));
        it->needs_files = hack || cJSON_IsTrue(cJSON_GetObjectItem(a, "requires_game_files"));
        if (hack && !*it->genre)
            copy_str(it->genre, sizeof(it->genre), "Rom hack");
        cJSON *num = cJSON_GetObjectItem(a, "size");
        it->size = cJSON_IsNumber(num) ? (long)num->valuedouble : 0;
        num = cJSON_GetObjectItem(a, "featured");
        it->featured = cJSON_IsNumber(num) ? num->valueint : 0;
        cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(a, "variants"))
        {
            if (cJSON_IsString(v) && it->variant_count < MAX_VARIANTS)
                copy_str(it->variants[it->variant_count++], sizeof(it->variants[0]), v->valuestring);
        }
        refresh_installed(it);
    }
    cJSON_Delete(root);
    qsort(items, item_count, sizeof(Item), cmp_items);

    // Systems present in the list, for the Select filter
    system_count = 0;
    sys_filter = -1;
    for (int i = 0; games && i < item_count; i++) {
        bool seen = false;
        for (int j = 0; j < system_count; j++)
            seen = seen || !strcmp(systems[j], items[i].system);
        if (!seen && system_count < MAX_SYSTEMS)
            copy_str(systems[system_count++], sizeof(systems[0]), items[i].system);
    }

    // Featured item that changes every day: a free port or homebrew game, or an app
    int free_ports[MAX_ITEMS], n = 0;
    for (int i = 0; i < item_count; i++)
        if (games ? ((items[i].kind == KIND_PORT && !items[i].needs_files) || items[i].kind == KIND_GAME)
                  : items[i].kind == KIND_APP)
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

/* ---------- lists ---------- */

static int tab_list(Tab t, int *out)
{
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        if (!passes_filter(it))
            continue;
        bool ok = (t == TAB_APPS && it->kind == (games ? KIND_GAME : KIND_APP)) || (t == TAB_PORTS && it->kind == KIND_PORT) ||
                  (t == TAB_THEMES && is_theme(it)) || (t == TAB_INSTALLED && *it->installed);
        if (ok)
            out[n++] = i;
    }
    return n;
}

static int cmp_featured(const void *a, const void *b)
{
    return items[*(const int *)a].featured - items[*(const int *)b].featured;
}

static int shelf_list(RowKind row, int *out)
{
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        if (!passes_filter(it))
            continue;
        if (row == ROW_PORTS && it->kind == KIND_PORT && !it->needs_files && i != hero)
            out[n++] = i;
        if (row == ROW_APPS && it->kind == (games ? KIND_GAME : KIND_APP) && i != hero)
            out[n++] = i;
        if (row == ROW_THEMES && is_theme(it) && it->featured > 0)
            out[n++] = i;
    }
    if (row == ROW_THEMES && n == 0) { // no featured list: show the first themes
        for (int i = 0; i < item_count && n < 8; i++)
            if (is_theme(&items[i]))
                out[n++] = i;
    }
    if (row == ROW_THEMES)
        qsort(out, n, sizeof(int), cmp_featured);
    return n;
}

static int shelf_slots(RowKind row)
{
    return row == ROW_THEMES ? 4 : 6;
}

// Items shown on a shelf; the last slot becomes "Se alle" when there are more
static int shelf_shown(RowKind row, int n)
{
    return n <= shelf_slots(row) ? n : shelf_slots(row) - 1;
}

static bool shelf_has_more(RowKind row, int n)
{
    return n > shelf_slots(row);
}

static Tab shelf_tab(RowKind row)
{
    return row == ROW_PORTS ? TAB_PORTS : (row == ROW_THEMES ? TAB_THEMES : TAB_APPS);
}

static int row_height(RowKind row)
{
    return row == ROW_HERO ? 210 : (row == ROW_THEMES ? 168 : 184);
}

static void build_feat_rows(void)
{
    static const RowKind all[] = {ROW_HERO, ROW_PORTS, ROW_THEMES, ROW_APPS};
    int list[MAX_ITEMS];
    feat_row_count = 0;
    for (int i = 0; i < 4; i++) {
        if (all[i] == ROW_HERO ? hero >= 0 && passes_filter(&items[hero]) : shelf_list(all[i], list) > 0)
            feat_rows[feat_row_count++] = all[i];
    }
}

/* ---------- jobs ---------- */

static void start_job(const char *kind, int idx, const char *arg2)
{
    if (job_pid > 0)
        return;
    copy_str(job_kind, sizeof(job_kind), kind);
    job_item = idx;
    char status_path[400];
    snprintf(status_path, sizeof(status_path), "%s/status", workdir);
    remove(status_path);
    job_status[0] = '\0';
    job_pid = spawn(kind, idx >= 0 ? items[idx].id : NULL, arg2);
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
    // The app's own name, not "onionsoup"
    soup_title(games ? "Game" : "App", "Store");

    int tx = SCREEN_W - 14;
    for (int j = TAB_N - 1; j >= 0; j--)
        tx -= text_w(f17, tab_name(tabs()[j])) + (j ? 22 : 0);
    for (int j = 0; j < TAB_N; j++) {
        int i = tabs()[j];
        int w = text_w(f17, tab_name(i));
        if (i == (int)tab && detail < 0) {
            fill_round(tx - 9, 12, w + 18, 28, 14, C_ACCENT);
            text(f17, tab_name(i), tx, 15, C_DARK);
        }
        else if (i == (int)tab) {
            fill_round(tx - 9, 12, w + 18, 28, 14, C_PANEL);
            text(f17, tab_name(i), tx, 15, C_ACCENT);
        }
        else {
            text(f17, tab_name(i), tx, 15, C_GREY);
        }
        tx += w + 22;
    }
}

static void pill(const char *label, int right_x, int y, SDL_Color bg, SDL_Color fg)
{
    int bw = text_w(f13, label) + 14;
    fill_round(right_x - bw, y, bw, 20, 10, bg);
    text(f13, label, right_x - bw + 7, y + 1, fg);
}

static void draw_badge(Item *it, int x, int y, int w)
{
    if (!*it->installed)
        return;
    bool update = has_update(it);
    bool in_use = active_variant(it) >= 0;
    pill(update ? "NY" : (in_use ? "I BRUK" : "OK"), x + w - 5, y + 5, update ? C_ACCENT : C_GREEN,
         update ? C_DARK : C_WHITE);
}

static void draw_card(int idx, int x, int y, bool selected)
{
    Item *it = &items[idx];
    int w, h;
    img_size(it, IMG_CARD, &w, &h);
    if (selected)
        outline_round(x - 5, y - 5, w + 10, h + 10, 12, 3, C_ACCENT, C_BG);
    draw_cover(idx, IMG_CARD, x, y);
    draw_badge(it, x, y, w);
    char name[160];
    short_name(it->name, name, sizeof(name));
    text_fit(f13, name, x, y + h + 6, w, selected ? C_WHITE : C_GREY);
}

// Last tile of a shelf: opens the full list
static void draw_see_all(int n, int x, int y, int w, int h, bool selected)
{
    if (selected)
        outline_round(x - 5, y - 5, w + 10, h + 10, 12, 3, C_ACCENT, C_BG);
    fill_round(x, y, w, h, 8, C_PANEL);
    const char *l1 = "Se alle";
    char l2[16];
    snprintf(l2, sizeof(l2), "%d", n);
    text(f17, l1, x + (w - text_w(f17, l1)) / 2, y + h / 2 - 22, selected ? C_ACCENT : C_WHITE);
    text(f15, l2, x + (w - text_w(f15, l2)) / 2, y + h / 2 + 2, C_GREY);
}

static void draw_hero(int y0)
{
    Item *it = &items[hero];
    fill(0, y0, SCREEN_W, 200, C_PANEL);
    SDL_Surface *big = item_img(hero, IMG_DETAIL);
    if (big) {
        // soft banner: stretched, darkened cover
        static SDL_Surface *band = NULL, *shade = NULL;
        static int band_for = -1;
        if (band_for != hero) {
            if (band)
                SDL_FreeSurface(band);
            band = scale_rounded(big, SCREEN_W, 200, 0);
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
    draw_cover(hero, IMG_HERO, 24, y0 + 10);
    text(f13, "UTVALGT", 172, y0 + 22, C_ACCENT);
    char name[160];
    short_name(it->name, name, sizeof(name));
    text_fit(f34, name, 172, y0 + 38, SCREEN_W - 190, C_WHITE);
    text_wrap(f15, it->description, 172, y0 + 84, SCREEN_W - 196, 20, 2, C_SOFT);
    char meta[128], sz[32];
    human_size(it->size, sz, sizeof(sz));
    snprintf(meta, sizeof(meta), "%s  ·  %s", *it->genre ? it->genre : (*it->system ? it->system : "App"), sz);
    text_fit(f15, meta, 172, y0 + 126, SCREEN_W - 196, C_GREY);
    const char *btn = *it->installed ? "Installert" : "Installer";
    int bw = text_w(f19, btn) + 40;
    if (feat_rows[feat_row] == ROW_HERO)
        outline_round(168, y0 + 150, bw + 8, 44, 22, 3, C_WHITE, C_BG);
    fill_round(172, y0 + 154, bw, 36, 18, *it->installed ? C_GREEN : C_ACCENT);
    text(f19, btn, 192, y0 + 160, *it->installed ? C_WHITE : C_DARK);
}

static void draw_featured(void)
{
    if (feat_row_count == 0) {
        const char *empty = "Ingen spill for dette systemet.";
        text(f17, empty, (SCREEN_W - text_w(f17, empty)) / 2, 230, C_GREY);
        return;
    }
    // Scroll so the selected row fits between the top bar and the hints
    int y_sel = 0;
    for (int r = 0; r < feat_row; r++)
        y_sel += row_height(feat_rows[r]);
    int scroll = 0;
    int view_h = BOTTOM_Y - TOP_H;
    if (y_sel + row_height(feat_rows[feat_row]) > view_h)
        scroll = y_sel + row_height(feat_rows[feat_row]) - view_h + 6;
    if (feat_row == 0)
        scroll = 0;

    const char *titles[] = {"", "Gratis porter", "Populære temaer", games ? "Homebrew" : "Apper"};
    int y = TOP_H - scroll;
    for (int r = 0; r < feat_row_count; r++) {
        RowKind row = feat_rows[r];
        int h = row_height(row);
        if (y + h < TOP_H || y > BOTTOM_Y) {
            y += h;
            continue;
        }
        if (row == ROW_HERO) {
            draw_hero(y);
            y += h;
            continue;
        }
        int list[MAX_ITEMS];
        int n = shelf_list(row, list);
        int shown = shelf_shown(row, n);
        y += row == feat_rows[0] ? 10 : 0;
        text(f19, titles[row], 24, y, C_WHITE);
        char count[16];
        snprintf(count, sizeof(count), "%d", n);
        text(f15, count, SCREEN_W - 24 - text_w(f15, count), y + 4, C_GREY);
        int cw, ch;
        img_size(&items[list[0]], IMG_CARD, &cw, &ch);
        int gap = row == ROW_THEMES ? 18 : 14;
        for (int k = 0; k < shown; k++)
            draw_card(list[k], 24 + k * (cw + gap), y + 32, feat_row == r && feat_col == k);
        if (shelf_has_more(row, n))
            draw_see_all(n, 24 + shown * (cw + gap), y + 32, cw, ch, feat_row == r && feat_col == shown);
        y += h - (row == feat_rows[0] ? 10 : 0);
    }
}

static void draw_scrollbar(int first, int visible, int total, int y, int h)
{
    if (total <= visible)
        return;
    int bar_h = h * visible / total;
    if (bar_h < 24)
        bar_h = 24;
    int max_first = total - visible;
    int bar_y = y + (h - bar_h) * (first > max_first ? max_first : first) / max_first;
    fill_round(SCREEN_W - 10, y, 4, h, 2, C_PANEL);
    fill_round(SCREEN_W - 10, bar_y, 4, bar_h, 2, C_ACCENT);
}

static void draw_list_header(const char *title, int n, int pos)
{
    text(f19, title, 24, TOP_H + 8, C_WHITE);
    char count[32];
    snprintf(count, sizeof(count), "%d", n);
    text(f15, count, 24 + text_w(f19, title) + 10, TOP_H + 12, C_GREY);
    if (n > 0) {
        char where[48];
        Item *it = &items[pos];
        char letter[2] = {(char)toupper((unsigned char)it->name[0]), 0};
        snprintf(where, sizeof(where), "%s  ·  %d av %d", letter, sel[tab] + 1, n);
        text(f15, where, SCREEN_W - 24 - text_w(f15, where), TOP_H + 12, C_GREY);
    }
}

static void meta_line(Item *it, char *out, size_t n)
{
    char sz[32];
    human_size(it->size, sz, sizeof(sz));
    const char *what = it->kind == KIND_APP ? "App"
                       : is_theme(it)       ? (it->variant_count > 1 ? "Temapakke" : "Tema")
                       : it->needs_files    ? "Egne spillfiler"
                                            : "Gratis";
    char extra[64] = "";
    if (*it->genre)
        snprintf(extra, sizeof(extra), "  ·  %s", it->genre);
    else if (is_theme(it) && *it->author)
        snprintf(extra, sizeof(extra), "  ·  %s", it->author);
    snprintf(out, n, "%s%s  ·  %s", what, extra, sz);
}

// One item per row: picture, name, details and status
static void draw_list(int *list, int n)
{
    int first = top[tab];
    for (int r = 0; r < LIST_ROWS && first + r < n; r++) {
        int i = first + r;
        Item *it = &items[list[i]];
        int y = LIST_Y + r * LIST_ROW_H;
        bool selected = i == sel[tab];
        if (selected)
            fill_round(14, y, SCREEN_W - 34, LIST_ROW_H - 4, 12, C_PANEL);
        int w, h;
        img_size(it, IMG_ROW, &w, &h);
        int px = 24 + (TROW_W - w) / 2;
        draw_cover(list[i], IMG_ROW, px, y + (LIST_ROW_H - 4 - h) / 2);
        int tx = 24 + TROW_W + 14;
        char name[160], meta[160];
        short_name(it->name, name, sizeof(name));
        meta_line(it, meta, sizeof(meta));
        int right = SCREEN_W - 34;
        if (*it->installed) {
            bool update = has_update(it);
            const char *label = update ? "Oppdatering" : (active_variant(it) >= 0 ? "I bruk" : "Installert");
            pill(label, right - 10, y + 17, update ? C_ACCENT : C_GREEN, update ? C_DARK : C_WHITE);
            right -= text_w(f13, label) + 34;
        }
        text_fit(f19, name, tx, y + 5, right - tx, selected ? C_WHITE : C_SOFT);
        text_fit(f13, meta, tx, y + 31, right - tx, selected ? C_ACCENT : C_GREY);
    }
    draw_scrollbar(first, LIST_ROWS, n, LIST_Y, LIST_ROWS * LIST_ROW_H - 4);
}

// Themes: big previews, three per row, scrolling up and down
static void draw_theme_grid(int *list, int n)
{
    int first_row = top[tab];
    int gap = (SCREEN_W - 48 - TGRID_COLS * TTILE_W) / (TGRID_COLS - 1);
    for (int r = 0; r < TGRID_ROWS; r++) {
        for (int c = 0; c < TGRID_COLS; c++) {
            int i = (first_row + r) * TGRID_COLS + c;
            if (i >= n)
                break;
            Item *it = &items[list[i]];
            int x = 24 + c * (TTILE_W + gap);
            int y = LIST_Y + 6 + r * TGRID_ROW_H;
            bool selected = i == sel[tab];
            if (selected)
                outline_round(x - 5, y - 5, TTILE_W + 10, TTILE_H + 10, 12, 3, C_ACCENT, C_BG);
            draw_cover(list[i], IMG_TILE, x, y);
            draw_badge(it, x, y, TTILE_W);
            char name[160];
            short_name(it->name, name, sizeof(name));
            text_fit(f15, name, x, y + TTILE_H + 6, TTILE_W, selected ? C_WHITE : C_GREY);
        }
    }
    int rows = (n + TGRID_COLS - 1) / TGRID_COLS;
    draw_scrollbar(first_row, TGRID_ROWS, rows, LIST_Y, TGRID_ROWS * TGRID_ROW_H - 10);
}

static bool tab_is_grid(Tab t)
{
    return t == TAB_THEMES;
}

// Keep the selection on screen
static void clamp_view(int n)
{
    if (sel[tab] >= n)
        sel[tab] = n ? n - 1 : 0;
    if (sel[tab] < 0)
        sel[tab] = 0;
    int per = tab_is_grid(tab) ? TGRID_COLS : 1;
    int vis = tab_is_grid(tab) ? TGRID_ROWS : LIST_ROWS;
    int row = sel[tab] / per;
    if (row < top[tab])
        top[tab] = row;
    if (row >= top[tab] + vis)
        top[tab] = row - vis + 1;
}

static void draw_tab_list(void)
{
    int list[MAX_ITEMS];
    int n = tab_list(tab, list);
    clamp_view(n);
    char title[64];
    if (sys_filter >= 0)
        snprintf(title, sizeof(title), "%s · %s", tab_name(tab), systems[sys_filter]);
    else
        copy_str(title, sizeof(title), tab_name(tab));
    draw_list_header(title, n, n ? list[sel[tab]] : 0);
    if (n == 0) {
        const char *empty = tab == TAB_INSTALLED ? (games ? "Ingenting installert fra Game Store ennå." : "Ingenting installert fra App Store ennå.") : "Ingenting her ennå.";
        text(f17, empty, (SCREEN_W - text_w(f17, empty)) / 2, 230, C_GREY);
        return;
    }
    if (tab_is_grid(tab))
        draw_theme_grid(list, n);
    else
        draw_list(list, n);
}

static void draw_detail(void)
{
    Item *it = &items[detail];
    bool theme = is_theme(it);
    int pw, ph;
    img_size(it, IMG_DETAIL, &pw, &ph);
    draw_cover(detail, IMG_DETAIL, 24, TOP_H + 18);

    int x = 24 + pw + 24, w = SCREEN_W - x - 24;
    char name[160];
    copy_str(name, sizeof(name), it->name);
    char *paren = strstr(name, " (");
    char engine[64] = "";
    if (paren && !theme) {
        copy_str(engine, sizeof(engine), paren + 2);
        engine[strcspn(engine, ")")] = '\0';
        *paren = '\0';
    }
    int y = text_wrap(theme ? f24 : f34, name, x, TOP_H + 14, w, theme ? 28 : 38, 2, C_WHITE);
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
    const char *kind = it->kind == KIND_APP ? "App" : theme         ? "Tema"
                                                  : it->needs_files ? "Egne spillfiler"
                                                                    : "Gratis";
    const char *chips[3] = {kind, *it->genre ? it->genre : NULL, sz};
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
    y += 36;
    y = text_wrap(f15, it->description, x, y, w, 21, theme ? 3 : 5, C_SOFT);

    // theme pack: pick the variant to use
    int in_use = active_variant(it);
    if (theme && it->variant_count > 1) {
        char v[160], line[200];
        short_name(it->variants[variant], v, sizeof(v));
        snprintf(line, sizeof(line), "‹  %s  ›", v);
        text(f13, "VARIANT", x, TOP_H + 168, C_ACCENT);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d av %d", variant + 1, it->variant_count);
        text(f13, pos, x + w - text_w(f13, pos), TOP_H + 168, C_GREY);
        fill_round(x, TOP_H + 186, w, 28, 14, C_CARD);
        text_fit(f15, line, x + 12, TOP_H + 189, w - 24, C_WHITE);
    }

    // action area
    int by_ = TOP_H + 222;
    char state[32], txt[128];
    int pct = 0;
    if (job_pid > 0 && job_item == detail) {
        parse_status(state, &pct, txt, sizeof(txt));
        fill_round(x, by_, w, 36, 18, C_PANEL);
        int bar = w * pct / 100;
        if (!strcmp(state, "download") && bar > 36)
            fill_round(x, by_, bar, 36, 18, C_ACCENT);
        char line[160];
        if (!strcmp(state, "download"))
            snprintf(line, sizeof(line), "Laster ned … %d %%", pct);
        else if (!strcmp(state, "verify"))
            snprintf(line, sizeof(line), "Sjekker filen …");
        else if (!strcmp(job_kind, "remove"))
            snprintf(line, sizeof(line), "Fjerner …");
        else if (!strcmp(job_kind, "apply"))
            snprintf(line, sizeof(line), "Bytter tema …");
        else
            snprintf(line, sizeof(line), "Installerer …");
        text(f17, line, x + 14, by_ + 7, !strcmp(state, "download") && bar > 120 ? C_DARK : C_WHITE);
    }
    else {
        const char *label;
        SDL_Color bg = C_ACCENT, fg = C_DARK;
        if (!*it->installed)
            label = "Installer";
        else if (has_update(it))
            label = "Oppdater";
        else if (theme && in_use == variant) {
            label = "I bruk";
            bg = C_GREEN;
            fg = C_WHITE;
        }
        else if (theme)
            label = "Bruk temaet";
        else {
            label = "Installert";
            bg = C_GREEN;
            fg = C_WHITE;
        }
        fill_round(x, by_, text_w(f19, label) + 40, 36, 18, bg);
        text(f19, label, x + 20, by_ + 6, fg);
    }

    int foot_y = TOP_H + 18 + ph + 18;
    if (foot_y < TOP_H + 290)
        foot_y = TOP_H + 290;
    if (!theme) {
        char foot[200];
        snprintf(foot, sizeof(foot), "Versjon %s%s%s", it->version, *it->installed ? "  ·  installert " : "",
                 *it->installed ? it->installed : "");
        text_fit(f13, foot, 24, foot_y, SCREEN_W - 48, C_GREY);
    }
    const char *note = NULL;
    if (it->kind == KIND_PORT)
        note = it->needs_files ? "Legg inn originale spillfiler, se _required_files.txt i Roms/PORTS/Games."
                               : "Krever «Ports Collection» i Package Manager. Kjør «~Import ports» etterpå.";
    else if (it->kind == KIND_GAME)
        note = it->needs_files ? "Rom hack: krever din egen original-ROM (ikke zippet) i Roms-mappen for systemet."
                               : "Gratis homebrew. Lagres i Roms-mappen for systemet.";
    else if (theme)
        note = "Fra OnionUI/Themes. Temaet kan også velges senere under Themes i Innstillinger.";
    if (note)
        text_fit(f13, note, 24, foot_y + (theme ? 0 : 22), SCREEN_W - 48, C_GREY);
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
    frame++;
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    if (detail >= 0)
        draw_detail();
    else if (tab == TAB_FEATURED)
        draw_featured();
    else
        draw_tab_list();
    draw_topbar();

    if (detail >= 0) {
        Item *it = &items[detail];
        bool pack = is_theme(it) && it->variant_count > 1;
        const char *k[4], *l[4];
        int n = 0;
        if (job_pid > 0) {
            k[n] = "B", l[n++] = "Tilbake";
        }
        else {
            if (!*it->installed)
                k[n] = "A", l[n++] = "Installer";
            else if (has_update(it))
                k[n] = "A", l[n++] = "Oppdater";
            else if (is_theme(it)) {
                if (active_variant(it) != variant)
                    k[n] = "A", l[n++] = "Bruk";
            }
            else
                k[n] = "A", l[n++] = "Installer på nytt";
            if (*it->installed)
                k[n] = "Y", l[n++] = "Fjern";
            if (pack)
                k[n] = "←→", l[n++] = "Variant";
            k[n] = "B", l[n++] = "Tilbake";
        }
        draw_hints(k, l, n);
    }
    else if (tab == TAB_FEATURED) {
        const char *k[] = {"A", "L/R", "SELECT", "B"};
        const char *l[] = {"Åpne", "Fane", "System", "Avslutt"};
        if (!games)
            k[2] = "B", l[2] = "Avslutt";
        draw_hints(k, l, games ? 4 : 3);
    }
    else {
        const char *k[] = {"A", "←→", "L2/R2", "L/R", "SELECT", "B"};
        const char *l[] = {"Åpne", tab_is_grid(tab) ? "Flytt" : "Side", "Bokstav", "Fane", "System", "Avslutt"};
        if (games)
            k[4] = "SELECT", l[4] = "System";
        else
            k[4] = "B", l[4] = "Avslutt";
        draw_hints(k, l, games ? 6 : 5);
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
    Item *it = &items[idx];
    int v = active_variant(it);
    variant = v >= 0 ? v : 0;
}

// Jump to the next (dir 1) or previous (dir -1) first letter
static void jump_letter(int *list, int n, int dir)
{
    int s = sel[tab];
    char cur = toupper((unsigned char)items[list[s]].name[0]);
    if (dir > 0) {
        while (s < n - 1 && toupper((unsigned char)items[list[s]].name[0]) == cur)
            s++;
    }
    else {
        // to the start of this letter, or of the previous one if already there
        if (s > 0 && toupper((unsigned char)items[list[s - 1]].name[0]) != cur)
            cur = toupper((unsigned char)items[list[--s]].name[0]);
        while (s > 0 && toupper((unsigned char)items[list[s - 1]].name[0]) == cur)
            s--;
    }
    sel[tab] = s;
}

static void on_key_detail(SDLKey key)
{
    Item *it = &items[detail];
    if (key == SW_BTN_B) {
        detail = -1;
        return;
    }
    if (job_pid > 0)
        return;
    if (is_theme(it) && it->variant_count > 1 && (key == SW_BTN_LEFT || key == SW_BTN_RIGHT)) {
        variant = (variant + (key == SW_BTN_RIGHT ? 1 : it->variant_count - 1)) % it->variant_count;
    }
    else if (key == SW_BTN_A) {
        if (is_theme(it) && *it->installed && !has_update(it)) {
            if (active_variant(it) != variant)
                start_job("apply", detail, it->variants[variant]);
        }
        else
            start_job("install", detail, NULL);
    }
    else if (key == SW_BTN_Y && *it->installed) {
        if (active_variant(it) >= 0)
            show_message("Temaet er i bruk. Bytt til et annet tema først.", 3500);
        else
            start_job("remove", detail, NULL);
    }
}

static void on_key_featured(SDLKey key)
{
    if (feat_row_count == 0)
        return;
    RowKind row = feat_rows[feat_row];
    int list[MAX_ITEMS];
    int n = row == ROW_HERO ? 0 : shelf_list(row, list);
    int slots = row == ROW_HERO ? 1 : shelf_shown(row, n) + (shelf_has_more(row, n) ? 1 : 0);
    if (key == SW_BTN_DOWN && feat_row < feat_row_count - 1) {
        feat_row++;
        feat_col = 0;
    }
    else if (key == SW_BTN_UP && feat_row > 0) {
        feat_row--;
        feat_col = 0;
    }
    else if (key == SW_BTN_RIGHT && feat_col < slots - 1)
        feat_col++;
    else if (key == SW_BTN_LEFT && feat_col > 0)
        feat_col--;
    else if (key == SW_BTN_A) {
        if (row == ROW_HERO)
            open_detail(hero);
        else if (feat_col < shelf_shown(row, n))
            open_detail(list[feat_col]);
        else { // "Se alle"
            tab = shelf_tab(row);
            sel[tab] = top[tab] = 0;
        }
    }
}

static void on_key_list(SDLKey key)
{
    int list[MAX_ITEMS];
    int n = tab_list(tab, list);
    if (!n)
        return;
    int *s = &sel[tab];
    bool grid = tab_is_grid(tab);
    int step = grid ? TGRID_COLS : 1;
    int page = grid ? TGRID_COLS * TGRID_ROWS : LIST_ROWS;
    if (key == SW_BTN_DOWN)
        *s = *s + step < n ? *s + step : (grid && *s / step < (n - 1) / step ? n - 1 : *s);
    else if (key == SW_BTN_UP && *s >= step)
        *s -= step;
    else if (key == SW_BTN_RIGHT)
        *s = grid ? (*s < n - 1 ? *s + 1 : *s) : (*s + page < n ? *s + page : n - 1);
    else if (key == SW_BTN_LEFT)
        *s = grid ? (*s > 0 ? *s - 1 : 0) : (*s > page ? *s - page : 0);
    else if (key == SW_BTN_R2)
        jump_letter(list, n, 1);
    else if (key == SW_BTN_L2)
        jump_letter(list, n, -1);
    else if (key == SW_BTN_A)
        open_detail(list[*s]);
    clamp_view(n);
}

static void on_key(SDLKey key)
{
    if (detail >= 0) {
        on_key_detail(key);
        return;
    }
    if (key == SW_BTN_B || key == SW_BTN_MENU) {
        quit = true;
        return;
    }
    if (key == SW_BTN_L1 || key == SW_BTN_R1) {
        int j = 0;
        while (j < TAB_N && tabs()[j] != tab)
            j++;
        tab = tabs()[(j + (key == SW_BTN_R1 ? 1 : TAB_N - 1)) % TAB_N];
        return;
    }
    if (games && key == SW_BTN_SELECT && system_count > 0) {
        sys_filter = sys_filter + 1 >= system_count ? -1 : sys_filter + 1;
        for (int t = 0; t < TAB_COUNT; t++)
            sel[t] = top[t] = 0;
        feat_row = feat_col = 0;
        build_feat_rows();
        show_message(sys_filter < 0 ? "Alle systemer" : systems[sys_filter], 1200);
        return;
    }
    if (tab == TAB_FEATURED)
        on_key_featured(key);
    else
        on_key_list(key);
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
    } keys[] = {{"U", SW_BTN_UP}, {"D", SW_BTN_DOWN}, {"L", SW_BTN_LEFT}, {"R", SW_BTN_RIGHT}, {"A", SW_BTN_A}, {"B", SW_BTN_B}, {"X", SW_BTN_X}, {"Y", SW_BTN_Y}, {"L1", SW_BTN_L1}, {"R1", SW_BTN_R1}, {"L2", SW_BTN_L2}, {"R2", SW_BTN_R2}};
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

static void job_finished(int code)
{
    if (job_item < 0)
        return;
    Item *it = &items[job_item];
    refresh_installed(it);
    read_active_theme();
    char state[32], txt[200];
    int pct;
    parse_status(state, &pct, txt, sizeof(txt));
    if (code != 0)
        show_message(*txt ? txt : "Noe gikk galt", 4000);
    else if (!strcmp(job_kind, "remove"))
        show_message("Fjernet", 2500);
    else if (!strcmp(job_kind, "apply"))
        show_message("Temaet er byttet. Avslutt for å se det.", 4000);
    else if (is_theme(it))
        show_message("Installert! Trykk A for å bruke temaet.", 3500);
    else
        show_message("Installert!", 2500);
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    const char *env;
    games = (argc > 1 && !strcmp(argv[1], "--games")) || getenv("GAMESTORE");
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
    read_active_theme();

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 60);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);

    if (!soup_open_fonts(font_path)) {
        fprintf(stderr, "appStore: cannot open font %s\n", font_path);
        return 1;
    }

    // Connect and download the catalog
    start_job("fetch", -1, NULL);
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
    build_feat_rows();

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
            job_finished(code);
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

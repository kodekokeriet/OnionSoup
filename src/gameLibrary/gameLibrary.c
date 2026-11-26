// OnionSoup game library: a visual way to browse and start games.
//
// The stock game lists draw box art on top of the game names. This program
// shows the same games as a cover grid (or a list with a preview panel), so
// art and text never overlap. Games are started through Onion's normal
// launch path (.tmp_update/cmd_to_run.sh), and the runtime brings the
// player back here when the game ends.
//
// Environment overrides for testing on a PC:
//   SDCARD              SD card root (default /mnt/SDCARD)
//   GAMELIB_FONT        TTF font
//   GAMELIB_SCRIPT      scripted input, e.g. "R R D A shot:out.bmp wait:500"

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <sqlite3/sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "soup/soup_ui.h"
#include "system/keymap_sw.h"

#define MAX_SYSTEMS 96
#define THUMB_CACHE 64

// Grid view
#define GRID_COLS 4
#define GRID_ROWS 2
#define CARD_W 140
#define ART_H 140
#define CARD_GAP_X 12
#define ROW_H 190
#define GRID_X ((SCREEN_W - GRID_COLS * CARD_W - (GRID_COLS - 1) * CARD_GAP_X) / 2)
#define GRID_Y 62

// Home view (console tiles)
#define TILE_COLS 3
#define TILE_W 194
#define TILE_H 176
#define TILE_ART_H 122
#define TILE_GAP 13
#define TILE_X ((SCREEN_W - TILE_COLS * TILE_W - (TILE_COLS - 1) * TILE_GAP) / 2)
#define TILE_Y 62

// List view
#define LIST_W 352
#define LIST_ROW_H 48
#define LIST_ROWS 8
#define LIST_Y 60

typedef struct {
    char *name;   // display name (from the game list or the file name)
    char *path;   // full rom path
    char *img;    // box art path ("" when unknown)
    char *launch; // launch script, NULL = the system's own
    int sys;      // index into systems[] (real system), -1 if unknown
} Game;

typedef struct {
    char key[64];  // folder name under Emu/, or "@recent" / "@fav"
    char name[96]; // friendly name
    char emu_dir[512];
    char rom_dir[512];
    char img_dir[512];
    char launch[512];
    char icon[512];
    char ext[256];
    Game *games;
    int count;
    int cap;
    bool virtual_list;
} System;

typedef struct {
    char key[600];
    SDL_Surface *surf;
    Uint32 used;
} Thumb;

enum { VIEW_HOME,
       VIEW_GAMES,
       VIEW_DETAIL };
enum { MODE_GRID,
       MODE_LIST };

static char sdcard[256] = "/mnt/SDCARD";
static char font_path[512];
static System systems[MAX_SYSTEMS];
static int system_count = 0;
static Thumb thumbs[THUMB_CACHE];
static Uint32 thumb_clock = 0;

static int view = VIEW_HOME;
static int mode = MODE_GRID;
static int home_sel = 0, home_scroll = 0;
static int cur_sys = -1;
static int sel = 0, scroll_row = 0;
static bool quit = false;
static bool launched = false;
static bool dirty = true;

static char msg[160];
static Uint32 msg_until = 0;

/* ---------- small helpers ---------- */

static char *dupstr(const char *s)
{
    return strdup(s ? s : "");
}

// realpath needs a PATH_MAX buffer; falls back to the input when it fails
static void resolve_path(const char *in, char *out, size_t n)
{
    char buf[PATH_MAX];
    copy_str(out, n, realpath(in, buf) ? buf : in);
}

static void path_join(char *out, size_t n, const char *dir, const char *rel)
{
    if (rel[0] == '/')
        copy_str(out, n, rel);
    else
        snprintf(out, n, "%s/%s", dir, rel);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void strip_ext(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot && dot != s)
        *dot = '\0';
}

// "Legend of Zelda, The - The Minish Cap (USA) [!]" -> "Legend of Zelda, The - The Minish Cap"
static void clean_title(const char *in, char *out, size_t n)
{
    copy_str(out, n, in);
    for (;;) {
        int len = strlen(out);
        while (len > 0 && out[len - 1] == ' ')
            out[--len] = '\0';
        if (len < 3)
            break;
        char close = out[len - 1];
        char open = close == ')' ? '(' : (close == ']' ? '[' : 0);
        if (!open)
            break;
        char *o = strrchr(out, open);
        if (!o || o == out)
            break;
        *o = '\0';
    }
    if (!*out)
        copy_str(out, n, in);
    // "Legend of Zelda, The - Minish Cap" -> "The Legend of Zelda - Minish Cap"
    char *the = strstr(out, ", The");
    if (the && (the[5] == '\0' || !strncmp(the + 5, " - ", 3) || !strncmp(the + 5, ": ", 2))) {
        char buf[512];
        *the = '\0';
        snprintf(buf, sizeof(buf), "The %s%s", out, the + 5);
        copy_str(out, n, buf);
    }
}

// Word wrap that puts an ellipsis on the last line when the text is too long
static void text_wrap_fit(TTF_Font *f, const char *s, int x, int y, int max_w, int line_h, int max_lines, SDL_Color c)
{
    const char *p = s;
    for (int line = 0; line < max_lines && *p; line++) {
        while (*p == ' ')
            p++;
        if (line == max_lines - 1) {
            text_fit(f, p, x, y, max_w, c);
            return;
        }
        // take as many words as fit
        const char *end = p, *best = NULL;
        while (*end) {
            const char *w = end;
            while (*w == ' ')
                w++;
            while (*w && *w != ' ')
                w++;
            char buf[256];
            int len = w - p < 255 ? (int)(w - p) : 255;
            memcpy(buf, p, len);
            buf[len] = '\0';
            if (text_w(f, buf) > max_w)
                break;
            best = w;
            end = w;
        }
        if (!best) { // a single word wider than the line
            text_fit(f, p, x, y, max_w, c);
            return;
        }
        char buf[256];
        int len = best - p < 255 ? (int)(best - p) : 255;
        memcpy(buf, p, len);
        buf[len] = '\0';
        text(f, buf, x, y, c);
        p = best;
        y += line_h;
    }
}

// Case-insensitive compare that ignores a leading "The "
static int title_cmp(const char *a, const char *b)
{
    if (!strncasecmp(a, "the ", 4))
        a += 4;
    if (!strncasecmp(b, "the ", 4))
        b += 4;
    return strcasecmp(a, b);
}

static int game_cmp(const void *pa, const void *pb)
{
    const Game *a = pa, *b = pb;
    return title_cmp(a->name, b->name);
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void show_message(const char *m, int ms)
{
    copy_str(msg, sizeof(msg), m);
    msg_until = SDL_GetTicks() + ms;
    dirty = true;
}

/* ---------- friendly system names ---------- */

static const char *friendly_name(const char *key, const char *label)
{
    static const struct {
        const char *key, *name;
    } names[] = {
        {"GB", "Game Boy"},
        {"GBC", "Game Boy Color"},
        {"GBA", "Game Boy Advance"},
        {"SGB", "Super Game Boy"},
        {"NDS", "Nintendo DS"},
        {"VB", "Virtual Boy"},
        {"POKE", "Pokémon Mini"},
        {"GW", "Game & Watch"},
        {"FC", "NES"},
        {"FDS", "Famicom Disk System"},
        {"SFC", "SNES"},
        {"SATELLAVIEW", "Satellaview"},
        {"SUFAMI", "Sufami Turbo"},
        {"MD", "Mega Drive"},
        {"MS", "Master System"},
        {"GG", "Game Gear"},
        {"SEGACD", "Sega CD"},
        {"THIRTYTWOX", "Sega 32X"},
        {"SEGASGONE", "SG-1000"},
        {"PSX", "PlayStation"},
        {"PCE", "PC Engine"},
        {"PCECD", "PC Engine CD"},
        {"SGFX", "SuperGrafx"},
        {"NEOGEO", "Neo Geo"},
        {"NEOCD", "Neo Geo CD"},
        {"NGP", "Neo Geo Pocket"},
        {"ARCADE", "Arcade"},
        {"CPS1", "Capcom CPS1"},
        {"CPS2", "Capcom CPS2"},
        {"CPS3", "Capcom CPS3"},
        {"ATARI", "Atari 2600"},
        {"FIFTYTWOHUNDRED", "Atari 5200"},
        {"SEVENTYEIGHTHUNDRED", "Atari 7800"},
        {"LYNX", "Atari Lynx"},
        {"WS", "WonderSwan"},
        {"PICO", "PICO-8"},
        {"TIC", "TIC-80"},
        {"PORTS", "Porter"},
        {"DOS", "MS-DOS"},
        {"SCUMMVM", "ScummVM"},
        {"MSX", "MSX"},
        {"COMMODORE", "Commodore 64"},
        {"VIC20", "VIC-20"},
        {"AMIGA", "Amiga"},
        {"CPC", "Amstrad CPC"},
        {"ZXS", "ZX Spectrum"},
        {"COLECO", "ColecoVision"},
        {"INTELLIVISION", "Intellivision"},
        {"VECTREX", "Vectrex"},
        {"ODYSSEY", "Odyssey²"},
        {"VIDEOPAC", "Videopac+"},
        {"FAIRCHILD", "Channel F"},
        {"MEGADUCK", "Mega Duck"},
        {"SUPERVISION", "Supervision"},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcasecmp(key, names[i].key))
            return names[i].name;
    return *label ? label : key;
}

/* ---------- loading games ---------- */

static void add_game(System *s, const char *name, const char *path, const char *img, const char *launch, int sys)
{
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->games = realloc(s->games, s->cap * sizeof(Game));
    }
    Game *g = &s->games[s->count++];
    g->name = dupstr(name);
    g->path = dupstr(path);
    g->img = dupstr(img);
    g->launch = launch ? dupstr(launch) : NULL;
    g->sys = sys;
}

static bool ext_ok(System *s, const char *file)
{
    const char *dot = strrchr(file, '.');
    if (!dot)
        return false;
    dot++;
    if (!strcasecmp(dot, "miyoocmd") || !strcasecmp(dot, "db") || !strcasecmp(dot, "xml") ||
        !strcasecmp(dot, "json") || !strcasecmp(dot, "txt") || !strcasecmp(dot, "cfg"))
        return false;
    if (!*s->ext)
        return true;
    char list[256];
    copy_str(list, sizeof(list), s->ext);
    for (char *t = strtok(list, "|"); t; t = strtok(NULL, "|"))
        if (!strcasecmp(t, dot))
            return true;
    return false;
}

// Names from miyoogamelist.xml: <game><path>./x.gba</path><name>X</name></game>
typedef struct {
    char file[256];
    char name[256];
} XmlName;

static XmlName *xml_names = NULL;
static int xml_count = 0;

static bool xml_tag(const char *block, const char *tag, char *out, size_t n)
{
    char open[32], close[32];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(block, open);
    if (!a)
        return false;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b)
        return false;
    size_t len = b - a < (long)n - 1 ? (size_t)(b - a) : n - 1;
    memcpy(out, a, len);
    out[len] = '\0';
    // the few entities a game name can hold
    char *p;
    while ((p = strstr(out, "&amp;"))) {
        memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
    while ((p = strstr(out, "&apos;"))) {
        *p = '\'';
        memmove(p + 1, p + 6, strlen(p + 6) + 1);
    }
    return true;
}

static void load_xml_names(const char *rom_dir)
{
    free(xml_names);
    xml_names = NULL;
    xml_count = 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/miyoogamelist.xml", rom_dir);
    char *buf = read_all(path);
    if (!buf)
        return;
    int cap = 0;
    for (char *p = strstr(buf, "<game>"); p; p = strstr(p + 6, "<game>")) {
        char *end = strstr(p, "</game>");
        if (!end)
            break;
        *end = '\0';
        XmlName x;
        char rel[256];
        if (xml_tag(p, "path", rel, sizeof(rel)) && xml_tag(p, "name", x.name, sizeof(x.name))) {
            copy_str(x.file, sizeof(x.file), base_name(rel));
            if (xml_count == cap) {
                cap = cap ? cap * 2 : 64;
                xml_names = realloc(xml_names, cap * sizeof(XmlName));
            }
            xml_names[xml_count++] = x;
        }
        *end = '<';
    }
    free(buf);
}

static const char *xml_lookup(const char *file)
{
    for (int i = 0; i < xml_count; i++)
        if (!strcmp(xml_names[i].file, file))
            return xml_names[i].name;
    return NULL;
}

static void scan_dir(System *s, int sys, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        bool dir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && is_dir(full));
        if (dir) {
            if (depth < 2 && strcasecmp(e->d_name, "Imgs") && strcasecmp(e->d_name, "Saves"))
                scan_dir(s, sys, full, depth + 1);
            continue;
        }
        if (!ext_ok(s, e->d_name))
            continue;
        char name[256], img[1024];
        const char *xn = depth == 0 ? xml_lookup(e->d_name) : NULL;
        copy_str(name, sizeof(name), xn ? xn : e->d_name);
        if (!xn)
            strip_ext(name);
        char stem[256];
        copy_str(stem, sizeof(stem), e->d_name);
        strip_ext(stem);
        snprintf(img, sizeof(img), "%s/%s.png", s->img_dir, stem);
        add_game(s, name, full, file_exists(img) ? img : "", NULL, sys);
    }
    closedir(d);
}

// MainUI keeps a cache of each list (with names from the game list) that we
// can read directly. It only exists for systems that have been opened there.
static bool load_from_cache(System *s, int sys)
{
    char db_path[700], table[80];
    const char *dir_name = base_name(s->rom_dir);
    snprintf(db_path, sizeof(db_path), "%s/%s_cache6.db", s->rom_dir, dir_name);
    if (!file_exists(db_path))
        return false;
    snprintf(table, sizeof(table), "%s_roms", dir_name);
    sqlite3 *db;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char *sql = sqlite3_mprintf("SELECT disp, path, imgpath FROM %q WHERE type=0 AND path NOT LIKE '%%.miyoocmd'", table);
    sqlite3_stmt *st;
    bool ok = sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    if (ok) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *disp = (const char *)sqlite3_column_text(st, 0);
            const char *path = (const char *)sqlite3_column_text(st, 1);
            const char *img = (const char *)sqlite3_column_text(st, 2);
            if (!path || !file_exists(path))
                continue;
            char name[256];
            if (disp && *disp)
                copy_str(name, sizeof(name), disp);
            else {
                copy_str(name, sizeof(name), base_name(path));
                strip_ext(name);
            }
            add_game(s, name, path, img && file_exists(img) ? img : "", NULL, sys);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok && s->count > 0;
}

static void load_system(const char *key)
{
    if (system_count >= MAX_SYSTEMS)
        return;
    System *s = &systems[system_count];
    memset(s, 0, sizeof(*s));
    copy_str(s->key, sizeof(s->key), key);
    snprintf(s->emu_dir, sizeof(s->emu_dir), "%s/Emu/%s", sdcard, key);

    char cfg_path[600];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", s->emu_dir);
    char *buf = read_all(cfg_path);
    if (!buf)
        return;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j)
        return;
    const char *rompath = cJSON_GetStringValue(cJSON_GetObjectItem(j, "rompath"));
    const char *launch = cJSON_GetStringValue(cJSON_GetObjectItem(j, "launch"));
    const char *imgpath = cJSON_GetStringValue(cJSON_GetObjectItem(j, "imgpath"));
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(j, "label"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(j, "icon"));
    const char *ext = cJSON_GetStringValue(cJSON_GetObjectItem(j, "extlist"));
    // Skip search results and anything without roms
    if (!rompath || !launch || !strncmp(rompath, "../../App/", 10)) {
        cJSON_Delete(j);
        return;
    }
    char tmp[1024];
    path_join(tmp, sizeof(tmp), s->emu_dir, rompath);
    resolve_path(tmp, s->rom_dir, sizeof(s->rom_dir));
    path_join(s->launch, sizeof(s->launch), s->emu_dir, launch);
    if (imgpath) {
        path_join(tmp, sizeof(tmp), s->emu_dir, imgpath);
        resolve_path(tmp, s->img_dir, sizeof(s->img_dir));
    }
    else
        snprintf(s->img_dir, sizeof(s->img_dir), "%s/Imgs", s->rom_dir);
    if (icon)
        path_join(s->icon, sizeof(s->icon), s->emu_dir, icon);
    copy_str(s->ext, sizeof(s->ext), ext);
    copy_str(s->name, sizeof(s->name), friendly_name(key, label ? label : ""));
    cJSON_Delete(j);

    if (!is_dir(s->rom_dir) || !file_exists(s->launch))
        return;

    int idx = system_count;
    if (!load_from_cache(s, idx)) {
        load_xml_names(s->rom_dir);
        scan_dir(s, idx, s->rom_dir, 0);
    }
    if (s->count == 0) {
        free(s->games);
        return;
    }
    qsort(s->games, s->count, sizeof(Game), game_cmp);
    system_count++;
}

static int sys_cmp(const void *pa, const void *pb)
{
    const System *a = pa, *b = pb;
    if (a->virtual_list != b->virtual_list)
        return a->virtual_list ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

// Find which real system a rom belongs to, from its launch script path
static int system_for_launch(const char *launch)
{
    for (int i = 0; i < system_count; i++) {
        if (systems[i].virtual_list)
            continue;
        size_t n = strlen(systems[i].emu_dir);
        if (!strncmp(launch, systems[i].emu_dir, n) && (launch[n] == '/' || !launch[n]))
            return i;
    }
    return -1;
}

static void load_json_list(const char *key, const char *name, const char *file, int max)
{
    if (system_count >= MAX_SYSTEMS)
        return;
    char path[600];
    snprintf(path, sizeof(path), "%s/Roms/%s", sdcard, file);
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    System *s = &systems[system_count];
    memset(s, 0, sizeof(*s));
    copy_str(s->key, sizeof(s->key), key);
    copy_str(s->name, sizeof(s->name), name);
    s->virtual_list = true;
    char line[4096];
    while (fgets(line, sizeof(line), fp) && s->count < max) {
        cJSON *j = cJSON_Parse(line);
        if (!j)
            continue;
        cJSON *type = cJSON_GetObjectItem(j, "type");
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(j, "label"));
        const char *rom = cJSON_GetStringValue(cJSON_GetObjectItem(j, "rompath"));
        const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(j, "imgpath"));
        const char *launch = cJSON_GetStringValue(cJSON_GetObjectItem(j, "launch"));
        // 5 = game, 17 = game with own launcher
        if (type && (type->valueint == 5 || type->valueint == 17) && rom && launch && file_exists(rom) &&
            !strstr(rom, ".miyoocmd")) {
            bool dup = false;
            char rom_real[1024], other[1024];
            resolve_path(rom, rom_real, sizeof(rom_real));
            for (int i = 0; i < s->count && !dup; i++) {
                resolve_path(s->games[i].path, other, sizeof(other));
                dup = !strcmp(other, rom_real);
            }
            if (!dup) {
                char real_launch[1024];
                resolve_path(launch, real_launch, sizeof(real_launch));
                int sys = system_for_launch(real_launch);
                char img_buf[1024] = "";
                if (img && file_exists(img))
                    copy_str(img_buf, sizeof(img_buf), img);
                else if (sys >= 0) {
                    char stem[256];
                    copy_str(stem, sizeof(stem), base_name(rom));
                    strip_ext(stem);
                    snprintf(img_buf, sizeof(img_buf), "%s/%s.png", systems[sys].img_dir, stem);
                    if (!file_exists(img_buf))
                        img_buf[0] = '\0';
                }
                char nm[256];
                if (label && *label)
                    copy_str(nm, sizeof(nm), label);
                else {
                    copy_str(nm, sizeof(nm), base_name(rom));
                    strip_ext(nm);
                }
                add_game(s, nm, rom, img_buf, launch, sys);
            }
        }
        cJSON_Delete(j);
    }
    fclose(fp);
    if (s->count == 0) {
        free(s->games);
        return;
    }
    system_count++;
}

static void load_all(void)
{
    char emu[600];
    snprintf(emu, sizeof(emu), "%s/Emu", sdcard);
    DIR *d = opendir(emu);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.')
                load_system(e->d_name);
        closedir(d);
    }
    free(xml_names);
    xml_names = NULL;

    // Recently played and favourites, as their own shelves at the start.
    // MainUI moves the recent list to a hidden file when recents are hidden.
    load_json_list("@recent", "Sist spilt", "recentlist.json", 24);
    if (system_count == 0 || strcmp(systems[system_count - 1].key, "@recent"))
        load_json_list("@recent", "Sist spilt", "recentlist-hidden.json", 24);
    load_json_list("@fav", "Favoritter", "favourite.json", 500);
    qsort(systems, system_count, sizeof(System), sys_cmp);
    // the order changed, so fix up the system index of every game
    for (int i = 0; i < system_count; i++) {
        if (systems[i].virtual_list)
            continue;
        for (int k = 0; k < systems[i].count; k++)
            systems[i].games[k].sys = i;
    }
    for (int i = 0; i < system_count; i++) {
        if (!systems[i].virtual_list)
            continue;
        for (int k = 0; k < systems[i].count; k++) {
            Game *g = &systems[i].games[k];
            char real_launch[1024];
            resolve_path(g->launch, real_launch, sizeof(real_launch));
            g->sys = system_for_launch(real_launch);
        }
    }
}

/* ---------- play activity ---------- */

typedef struct {
    int seconds;
    int count;
    long last;
} PlayStats;

static void play_stats(const Game *g, PlayStats *out)
{
    memset(out, 0, sizeof(*out));
    char db_path[600];
    snprintf(db_path, sizeof(db_path), "%s/Saves/CurrentProfile/play_activity/play_activity_db.sqlite", sdcard);
    if (!file_exists(db_path))
        return;
    // the database stores paths relative to Roms/
    char roms[300];
    snprintf(roms, sizeof(roms), "%s/Roms/", sdcard);
    char real[1024];
    resolve_path(g->path, real, sizeof(real));
    const char *rel = !strncmp(real, roms, strlen(roms)) ? real + strlen(roms) : real;
    sqlite3 *db;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    char *sql = sqlite3_mprintf("SELECT SUM(a.play_time), COUNT(a.play_time), MAX(a.created_at) FROM play_activity a "
                                "JOIN rom r ON r.id = a.rom_id WHERE r.file_path = %Q",
                                rel);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->seconds = sqlite3_column_int(st, 0);
            out->count = sqlite3_column_int(st, 1);
            out->last = (long)sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    }
    sqlite3_free(sql);
    sqlite3_close(db);
}

/* ---------- thumbnails ---------- */

// Scale an image to fit inside box_w x box_h (contain), or to fill it exactly
// and crop the overflow (cover).
static SDL_Surface *make_thumb(const char *path, int box_w, int box_h, bool cover, int radius)
{
    SDL_Surface *img = IMG_Load(path);
    if (!img)
        return NULL;
    SDL_Surface *src = img;
    SDL_Surface *crop = NULL;
    int w, h;
    if (cover) {
        // crop the source to the box's aspect ratio, keeping the top part
        // (titles and logos usually sit there)
        float box_ar = (float)box_w / box_h;
        float img_ar = (float)img->w / img->h;
        SDL_Rect r = {0, 0, img->w, img->h};
        if (img_ar > box_ar) {
            r.w = (Uint16)(img->h * box_ar);
            r.x = (img->w - r.w) / 2;
        }
        else {
            r.h = (Uint16)(img->w / box_ar);
            r.y = (img->h - r.h) / 4;
        }
        crop = SDL_CreateRGBSurface(SDL_SWSURFACE, r.w, r.h, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (crop) {
            SDL_SetAlpha(img, 0, 255);
            SDL_BlitSurface(img, &r, crop, NULL);
            src = crop;
        }
        w = box_w;
        h = box_h;
    }
    else {
        float s = fminf((float)box_w / img->w, (float)box_h / img->h);
        w = (int)(img->w * s + 0.5f);
        h = (int)(img->h * s + 0.5f);
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
    }
    SDL_Surface *out = scale_rounded(src, w, h, radius);
    if (crop)
        SDL_FreeSurface(crop);
    SDL_FreeSurface(img);
    return out;
}

// Returns a cached thumbnail, or NULL if it is not loaded yet. When `load` is
// true a missing thumbnail is loaded now.
static SDL_Surface *thumb(const char *path, int w, int h, bool cover, int radius, bool load, bool *pending)
{
    if (!path || !*path)
        return NULL;
    char key[600];
    snprintf(key, sizeof(key), "%d:%d:%d:%s", w, h, cover, path);
    for (int i = 0; i < THUMB_CACHE; i++) {
        if (thumbs[i].used && !strcmp(thumbs[i].key, key)) {
            thumbs[i].used = ++thumb_clock;
            return thumbs[i].surf;
        }
    }
    if (!load) {
        if (pending)
            *pending = true;
        return NULL;
    }
    int slot = 0;
    for (int i = 1; i < THUMB_CACHE; i++)
        if (thumbs[i].used < thumbs[slot].used)
            slot = i;
    if (thumbs[slot].surf)
        SDL_FreeSurface(thumbs[slot].surf);
    copy_str(thumbs[slot].key, sizeof(thumbs[slot].key), key);
    // a failed load is cached as NULL so it is not retried every frame
    thumbs[slot].surf = make_thumb(path, w, h, cover, radius);
    thumbs[slot].used = ++thumb_clock;
    return thumbs[slot].surf;
}

// One image is loaded per frame so the screen stays responsive while art
// streams in.
static bool load_budget = true;

static SDL_Surface *thumb_lazy(const char *path, int w, int h, bool cover, int radius)
{
    bool pending = false;
    SDL_Surface *s = thumb(path, w, h, cover, radius, false, &pending);
    if (!s && pending && load_budget) {
        load_budget = false;
        s = thumb(path, w, h, cover, radius, true, NULL);
        dirty = true;
    }
    else if (pending)
        dirty = true;
    return s;
}

static void blit_at(SDL_Surface *s, int x, int y)
{
    SDL_Rect r = {x, y};
    SDL_BlitSurface(s, NULL, screen, &r);
}

/* ---------- drawing ---------- */

static SDL_Color mix(SDL_Color a, SDL_Color b, float t)
{
    SDL_Color c = {(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t), (Uint8)(a.b + (b.b - a.b) * t)};
    return c;
}

// Placeholder for games without art: a tinted card with the title on it
static void draw_placeholder(const Game *g, int x, int y, int w, int h, bool selected)
{
    // a stable colour per title
    unsigned hash = 5381;
    for (const char *p = g->name; *p; p++)
        hash = hash * 33 + (unsigned char)*p;
    static const SDL_Color tints[] = {{120, 72, 52}, {70, 92, 120}, {88, 110, 70}, {112, 70, 110}, {120, 100, 52}, {60, 104, 104}};
    SDL_Color tint = tints[hash % 6];
    fill_round(x, y, w, h, 10, mix(C_CARD, tint, selected ? 0.75f : 0.55f));
    char title[256];
    clean_title(g->name, title, sizeof(title));
    TTF_Font *f = w >= 200 ? f24 : f17;
    int line_h = w >= 200 ? 30 : 22;
    int max_lines = (h - 24) / line_h;
    if (max_lines > 5)
        max_lines = 5;
    // vertically centre the wrapped text: measure by drawing off screen first
    int lines = 0;
    {
        char line[256] = "", word[128];
        const char *p = title;
        while (*p) {
            int n = 0;
            while (*p == ' ')
                p++;
            while (*p && *p != ' ' && n < 127)
                word[n++] = *p++;
            word[n] = '\0';
            if (!n)
                break;
            char trial[400];
            snprintf(trial, sizeof(trial), "%s%s%s", line, *line ? " " : "", word);
            if (text_w(f, trial) > w - 20 && *line) {
                lines++;
                copy_str(line, sizeof(line), word);
            }
            else
                copy_str(line, sizeof(line), trial);
        }
        if (*line)
            lines++;
    }
    if (lines > max_lines)
        lines = max_lines;
    int ty = y + (h - lines * line_h) / 2;
    text_wrap(f, title, x + 10, ty, w - 20, line_h, max_lines, C_WHITE);
}

// Draw a game's art centred in a box; returns false if a placeholder was drawn
static void draw_art(const Game *g, int x, int y, int w, int h, bool selected)
{
    SDL_Surface *t = thumb_lazy(g->img, w, h, false, 8);
    if (t) {
        fill_round(x, y, w, h, 10, C_PANEL);
        blit_at(t, x + (w - t->w) / 2, y + (h - t->h) / 2);
    }
    else
        draw_placeholder(g, x, y, w, h, selected);
}

static void draw_topbar(const char *subtitle, const char *right)
{
    soup_logo(subtitle);
    if (right && *right)
        text(f17, right, SCREEN_W - 18 - text_w(f17, right), 16, C_GREY);
}

static void draw_scrollbar(int first, int visible, int total, int x, int y, int h)
{
    if (total <= visible)
        return;
    fill_round(x, y, 4, h, 2, C_CARD);
    int bh = h * visible / total;
    if (bh < 20)
        bh = 20;
    int by = y + (h - bh) * first / (total - visible);
    fill_round(x, by, 4, bh, 2, C_ACCENT);
}

static void draw_message(void)
{
    if (!*msg || SDL_GetTicks() > msg_until)
        return;
    int w = text_w(f17, msg) + 40;
    fill_round((SCREEN_W - w) / 2, 392, w, 40, 20, C_ACCENT);
    text(f17, msg, (SCREEN_W - w) / 2 + 20, 401, C_DARK);
}

/* home: one tile per console */

static const Game *tile_cover(const System *s)
{
    for (int i = 0; i < s->count; i++)
        if (*s->games[i].img)
            return &s->games[i];
    return NULL;
}

static void draw_tile(int idx, int x, int y, bool selected)
{
    System *s = &systems[idx];
    if (selected)
        fill_round(x - 4, y - 4, TILE_W + 8, TILE_H + 8, 16, C_ACCENT);
    fill_round(x, y, TILE_W, TILE_H, 12, C_PANEL);

    // Art strip: up to three covers side by side, or the console icon
    int art_y = y + 8, art_h = TILE_ART_H - 8;
    const Game *covers[3];
    int n = 0;
    for (int i = 0; i < s->count && n < 3; i++)
        if (*s->games[i].img)
            covers[n++] = &s->games[i];
    if (n > 0) {
        // Covers keep their shape and fan out from the left; when they do
        // not fit side by side they overlap, front cover first.
        fill_round(x + 8, art_y, TILE_W - 16, art_h, 8, C_CARD);
        int avail = TILE_W - 28;
        SDL_Surface *ts[3];
        int widths = 0;
        for (int i = 0; i < n; i++) {
            ts[i] = thumb_lazy(covers[i]->img, n == 1 ? avail : 104, art_h - 12, false, 6);
            widths += ts[i] ? ts[i]->w : 80;
        }
        int gap = 6;
        int step_extra = 0;
        if (widths + gap * (n - 1) > avail && n > 1) {
            gap = 0;
            step_extra = (avail - widths) / (n - 1); // negative: overlap
        }
        int total = widths + (gap + step_extra) * (n - 1);
        int cx = x + 14 + (avail - total) / 2;
        int pos[3];
        for (int i = 0; i < n; i++) {
            pos[i] = cx;
            cx += (ts[i] ? ts[i]->w : 80) + gap + step_extra;
        }
        for (int i = n - 1; i >= 0; i--) {
            int w = ts[i] ? ts[i]->w : 80;
            int h = ts[i] ? ts[i]->h : art_h - 12;
            int ty = art_y + (art_h - h) / 2;
            // a dark edge so overlapping covers stay apart
            fill_round(pos[i] - 2, ty - 2, w + 4, h + 4, 8, C_BAR);
            if (ts[i])
                blit_at(ts[i], pos[i], ty);
            else
                fill_round(pos[i], ty, w, h, 6, C_PANEL);
        }
    }
    else {
        fill_round(x + 8, art_y, TILE_W - 16, art_h, 8, C_CARD);
        SDL_Surface *ic = *s->icon ? thumb_lazy(s->icon, 96, art_h - 16, false, 0) : NULL;
        if (ic)
            blit_at(ic, x + (TILE_W - ic->w) / 2, art_y + (art_h - ic->h) / 2);
        else {
            const char *glyph = s->virtual_list ? (s->key[1] == 'f' ? "★" : "↺") : "";
            if (*glyph)
                text(f34, glyph, x + (TILE_W - text_w(f34, glyph)) / 2, art_y + art_h / 2 - 20, C_ACCENT);
        }
    }

    text_fit(f19, s->name, x + 12, y + TILE_ART_H + 6, TILE_W - 24, selected ? C_WHITE : C_SOFT);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d spill", s->count);
    text(f15, cnt, x + 12, y + TILE_ART_H + 30, s->virtual_list ? C_ACCENT : C_GREY);
}

static void draw_home(void)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    int total = 0;
    for (int i = 0; i < system_count; i++)
        if (!systems[i].virtual_list)
            total += systems[i].count;
    char right[48];
    snprintf(right, sizeof(right), "%d spill", total);
    draw_topbar("Spill", right);

    if (system_count == 0) {
        const char *t = "Fant ingen spill";
        text(f24, t, (SCREEN_W - text_w(f24, t)) / 2, 190, C_WHITE);
        text_wrap(f15, "Legg spill i Roms-mappen på minnekortet, så dukker de opp her.", 120, 230, 400, 20, 3, C_GREY);
    }
    else {
        int rows_visible = 2;
        for (int r = 0; r < rows_visible + 1; r++) {
            for (int c = 0; c < TILE_COLS; c++) {
                int idx = (home_scroll + r) * TILE_COLS + c;
                if (idx >= system_count)
                    break;
                int x = TILE_X + c * (TILE_W + TILE_GAP);
                int y = TILE_Y + r * (TILE_H + 16);
                if (y + 40 > BOTTOM_Y)
                    break;
                draw_tile(idx, x, y, idx == home_sel);
            }
        }
        int rows = (system_count + TILE_COLS - 1) / TILE_COLS;
        draw_scrollbar(home_scroll, rows_visible, rows, SCREEN_W - 7, TILE_Y, BOTTOM_Y - TILE_Y - 10);
        // fade the half row at the bottom into the hint bar
        fill(0, BOTTOM_Y - 6, SCREEN_W, 6, C_BG);
    }

    const char *k[] = {"A", "B", "Y"};
    const char *l[] = {"Åpne", "Avslutt", mode == MODE_GRID ? "Listevisning" : "Rutenett"};
    draw_hints(k, l, 3);
}

/* games: grid of covers */

static void draw_grid(System *s)
{
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int idx = (scroll_row + r) * GRID_COLS + c;
            if (idx >= s->count)
                break;
            Game *g = &s->games[idx];
            int x = GRID_X + c * (CARD_W + CARD_GAP_X);
            int y = GRID_Y + r * ROW_H;
            bool on = idx == sel;
            if (on)
                fill_round(x - 5, y - 5, CARD_W + 10, ART_H + 10, 14, C_ACCENT);
            draw_art(g, x, y, CARD_W, ART_H, on);
            char title[256];
            clean_title(g->name, title, sizeof(title));
            text_wrap_fit(f15, title, x + 2, y + ART_H + 8, CARD_W - 4, 18, 2, on ? C_WHITE : C_GREY);
        }
    }
    int rows = (s->count + GRID_COLS - 1) / GRID_COLS;
    draw_scrollbar(scroll_row, GRID_ROWS, rows, SCREEN_W - 7, GRID_Y, GRID_ROWS * ROW_H - 16);
}

/* games: list with a preview panel */

static void draw_list(System *s)
{
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = scroll_row + r;
        if (idx >= s->count)
            break;
        Game *g = &s->games[idx];
        int y = LIST_Y + r * LIST_ROW_H;
        bool on = idx == sel;
        if (on)
            fill_round(10, y, LIST_W, LIST_ROW_H - 4, 10, C_CARD);
        if (on)
            fill_round(10, y + 8, 4, LIST_ROW_H - 20, 2, C_ACCENT);
        // small thumbnail, never under the text
        SDL_Surface *t = thumb_lazy(g->img, 36, 36, false, 4);
        if (t)
            blit_at(t, 22 + (36 - t->w) / 2, y + 4 + (36 - t->h) / 2);
        else
            fill_round(22, y + 4, 36, 36, 6, C_PANEL);
        char title[256];
        clean_title(g->name, title, sizeof(title));
        text_fit(f19, title, 68, y + 9, LIST_W - 70, on ? C_WHITE : C_SOFT);
    }
    draw_scrollbar(scroll_row, LIST_ROWS, s->count, LIST_W + 16, LIST_Y, LIST_ROWS * LIST_ROW_H - 4);

    // preview of the selected game
    if (sel < s->count) {
        Game *g = &s->games[sel];
        int px = LIST_W + 32, pw = SCREEN_W - px - 16;
        draw_art(g, px, LIST_Y, pw, 230, true);
        text_wrap_fit(f19, g->name, px, LIST_Y + 242, pw, 23, 3, C_WHITE);
        if (g->sys >= 0 && s->virtual_list)
            text_fit(f15, systems[g->sys].name, px, LIST_Y + 316, pw, C_ACCENT);
    }
}

static void draw_games(void)
{
    System *s = &systems[cur_sys];
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    char right[48];
    snprintf(right, sizeof(right), "%d / %d", sel + 1, s->count);
    draw_topbar(s->name, right);
    if (mode == MODE_GRID)
        draw_grid(s);
    else
        draw_list(s);
    const char *k[] = {"A", "B", "X", "Y", "L/R"};
    const char *l[] = {"Spill", "Tilbake", "Info", mode == MODE_GRID ? "Liste" : "Rutenett", "Bla"};
    draw_hints(k, l, 5);
}

/* detail */

static void draw_detail(void)
{
    System *s = &systems[cur_sys];
    Game *g = &s->games[sel];
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    draw_topbar(s->name, "");

    int ax = 24, ay = 70, aw = 260, ah = 300;
    SDL_Surface *t = thumb(g->img, aw, ah, false, 12, true, NULL);
    if (t)
        blit_at(t, ax + (aw - t->w) / 2, ay + (ah - t->h) / 2);
    else
        draw_placeholder(g, ax, ay, aw, ah, true);

    int x = 310, w = SCREEN_W - x - 24;
    char full[256];
    clean_title(g->name, full, sizeof(full));
    int y = text_wrap(f24, full, x, 70, w, 30, 4, C_WHITE);
    y += 6;
    if (g->sys >= 0)
        text_fit(f17, systems[g->sys].name, x, y, w, C_ACCENT);
    y += 34;

    static PlayStats ps;
    static const Game *ps_for = NULL;
    if (ps_for != g) {
        play_stats(g, &ps);
        ps_for = g;
    }
    char line[96];
    if (ps.count > 0) {
        int h = ps.seconds / 3600, m = (ps.seconds % 3600) / 60;
        if (h > 0)
            snprintf(line, sizeof(line), "Spilt %d t %d min", h, m);
        else
            snprintf(line, sizeof(line), "Spilt %d min", m > 0 ? m : 1);
        text(f17, line, x, y, C_SOFT);
        y += 26;
        if (ps.last > 0) {
            time_t lt = (time_t)ps.last;
            struct tm *tm = localtime(&lt);
            strftime(line, sizeof(line), "Sist spilt %d.%m.%Y", tm);
            text(f17, line, x, y, C_SOFT);
            y += 26;
        }
    }
    else {
        text(f17, "Ikke spilt ennå", x, y, C_GREY);
        y += 26;
    }
    struct stat st;
    if (stat(g->path, &st) == 0) {
        char sz[32];
        human_size((long)st.st_size, sz, sizeof(sz));
        snprintf(line, sizeof(line), "Størrelse %s", sz);
        text(f17, line, x, y, C_GREY);
        y += 26;
    }
    text_wrap(f13, base_name(g->path), x, y + 8, w, 17, 3, C_GREY);

    // big play button
    fill_round(x, 330, 170, 44, 22, C_ACCENT);
    const char *play = "Spill";
    text(f24, play, x + (170 - text_w(f24, play)) / 2, 337, C_DARK);

    const char *k[] = {"A", "B"};
    const char *l[] = {"Spill", "Tilbake"};
    draw_hints(k, l, 2);
}

static void draw(void)
{
    load_budget = true;
    dirty = false;
    if (view == VIEW_HOME)
        draw_home();
    else if (view == VIEW_GAMES)
        draw_games();
    else
        draw_detail();
    draw_message();
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

/* ---------- state ---------- */

static void state_path(char *out, size_t n)
{
    snprintf(out, n, "%s/.tmp_update/config/gameLibrary.cfg", sdcard);
}

static void save_state(void)
{
    char p[600];
    state_path(p, sizeof(p));
    FILE *fp = fopen(p, "w");
    if (!fp)
        return;
    fprintf(fp, "mode=%d\n", mode);
    if (view != VIEW_HOME && cur_sys >= 0) {
        fprintf(fp, "system=%s\n", systems[cur_sys].key);
        fprintf(fp, "game=%s\n", systems[cur_sys].games[sel].path);
    }
    else if (home_sel < system_count)
        fprintf(fp, "home=%s\n", systems[home_sel].key);
    fclose(fp);
}

static void fix_scroll(void);

static void load_state(void)
{
    char p[600];
    state_path(p, sizeof(p));
    FILE *fp = fopen(p, "r");
    if (!fp)
        return;
    char line[1100], sys_key[64] = "", game[1024] = "", home[64] = "";
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "mode=", 5))
            mode = atoi(line + 5) == MODE_LIST ? MODE_LIST : MODE_GRID;
        else if (!strncmp(line, "system=", 7))
            copy_str(sys_key, sizeof(sys_key), line + 7);
        else if (!strncmp(line, "game=", 5))
            copy_str(game, sizeof(game), line + 5);
        else if (!strncmp(line, "home=", 5))
            copy_str(home, sizeof(home), line + 5);
    }
    fclose(fp);
    for (int i = 0; i < system_count; i++) {
        if (*home && !strcmp(systems[i].key, home))
            home_sel = i;
        if (*sys_key && !strcmp(systems[i].key, sys_key)) {
            home_sel = cur_sys = i;
            view = VIEW_GAMES;
            sel = 0;
            for (int k = 0; k < systems[i].count; k++)
                if (!strcmp(systems[i].games[k].path, game))
                    sel = k;
        }
    }
    fix_scroll();
}

/* ---------- launching ---------- */

static void add_to_recents(const Game *g, const char *launch)
{
    char rl[600], hidden_flag[600];
    snprintf(hidden_flag, sizeof(hidden_flag), "%s/.tmp_update/config/.showRecents", sdcard);
    snprintf(rl, sizeof(rl), "%s/Roms/%s", sdcard, file_exists(hidden_flag) ? "recentlist.json" : "recentlist-hidden.json");

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "label", g->name);
    cJSON_AddStringToObject(j, "rompath", g->path);
    if (*g->img)
        cJSON_AddStringToObject(j, "imgpath", g->img);
    cJSON_AddStringToObject(j, "launch", launch);
    cJSON_AddNumberToObject(j, "type", 5);
    char *first = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!first)
        return;

    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", rl);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        free(first);
        return;
    }
    fprintf(out, "%s\n", first);
    free(first);
    char me[1024];
    resolve_path(g->path, me, sizeof(me));
    FILE *in = fopen(rl, "r");
    if (in) {
        char line[4096];
        int kept = 0;
        while (fgets(line, sizeof(line), in) && kept < 49) {
            cJSON *e = cJSON_Parse(line);
            const char *rp = e ? cJSON_GetStringValue(cJSON_GetObjectItem(e, "rompath")) : NULL;
            bool same = false;
            if (rp) {
                // the same file can be written as .../Emu/X/../../Roms/X/...
                char a[1024];
                resolve_path(rp, a, sizeof(a));
                same = !strcmp(a, me);
            }
            cJSON_Delete(e);
            if (same || line[0] == '\n')
                continue;
            fputs(line, out);
            if (line[strlen(line) - 1] != '\n')
                fputc('\n', out);
            kept++;
        }
        fclose(in);
    }
    fclose(out);
    rename(tmp, rl);
}

static void launch_game(const Game *g)
{
    const char *launch = g->launch ? g->launch : (g->sys >= 0 ? systems[g->sys].launch : NULL);
    if (!launch || !file_exists(g->path)) {
        show_message("Fant ikke spillet", 2500);
        return;
    }
    char cmd_path[600];
    snprintf(cmd_path, sizeof(cmd_path), "%s/.tmp_update/cmd_to_run.sh", sdcard);
    FILE *fp = fopen(cmd_path, "w");
    if (!fp) {
        show_message("Kunne ikke starte spillet", 2500);
        return;
    }
    fprintf(fp, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, g->path);
    fclose(fp);
    add_to_recents(g, launch);
    // Tell the runtime to start the game now, and to come back here after
    FILE *f = fopen("/tmp/quick_switch", "w");
    if (f)
        fclose(f);
    f = fopen("/tmp/soup_library_return", "w");
    if (f)
        fclose(f);
    sync();
    launched = true;
    quit = true;
}

/* ---------- input ---------- */

static void fix_scroll(void)
{
    if (view == VIEW_HOME) {
        int row = home_sel / TILE_COLS;
        if (row < home_scroll)
            home_scroll = row;
        if (row > home_scroll + 1)
            home_scroll = row - 1;
        return;
    }
    if (cur_sys < 0)
        return;
    if (mode == MODE_GRID) {
        int row = sel / GRID_COLS;
        if (row < scroll_row)
            scroll_row = row;
        if (row >= scroll_row + GRID_ROWS)
            scroll_row = row - GRID_ROWS + 1;
    }
    else {
        if (sel < scroll_row)
            scroll_row = sel;
        if (sel >= scroll_row + LIST_ROWS)
            scroll_row = sel - LIST_ROWS + 1;
    }
}

// Jump to the next / previous first letter (L1/R1)
static void jump_letter(System *s, int dir)
{
    if (s->count == 0)
        return;
    char title[256];
    clean_title(s->games[sel].name, title, sizeof(title));
    const char *t = strncasecmp(title, "the ", 4) ? title : title + 4;
    int cur = toupper((unsigned char)t[0]);
    int i = sel;
    for (;;) {
        i += dir;
        if (i < 0 || i >= s->count) {
            i = dir > 0 ? s->count - 1 : 0;
            break;
        }
        clean_title(s->games[i].name, title, sizeof(title));
        t = strncasecmp(title, "the ", 4) ? title : title + 4;
        if (toupper((unsigned char)t[0]) != cur) {
            if (dir < 0) {
                // go to the first game of that letter
                int letter = toupper((unsigned char)t[0]);
                while (i > 0) {
                    clean_title(s->games[i - 1].name, title, sizeof(title));
                    t = strncasecmp(title, "the ", 4) ? title : title + 4;
                    if (toupper((unsigned char)t[0]) != letter)
                        break;
                    i--;
                }
            }
            break;
        }
    }
    sel = i;
}

static void on_key(SDLKey key)
{
    dirty = true;
    if (key == SW_BTN_MENU) {
        quit = true;
        return;
    }
    if (view == VIEW_HOME) {
        if (key == SW_BTN_B) {
            quit = true;
            return;
        }
        if (key == SW_BTN_Y) {
            mode = mode == MODE_GRID ? MODE_LIST : MODE_GRID;
            return;
        }
        if (!system_count)
            return;
        if (key == SW_BTN_RIGHT && home_sel < system_count - 1)
            home_sel++;
        else if (key == SW_BTN_LEFT && home_sel > 0)
            home_sel--;
        else if (key == SW_BTN_DOWN && home_sel + TILE_COLS < system_count)
            home_sel += TILE_COLS;
        else if (key == SW_BTN_DOWN && home_sel / TILE_COLS < (system_count - 1) / TILE_COLS)
            home_sel = system_count - 1;
        else if (key == SW_BTN_UP && home_sel >= TILE_COLS)
            home_sel -= TILE_COLS;
        else if (key == SW_BTN_A || key == SW_BTN_START) {
            cur_sys = home_sel;
            sel = scroll_row = 0;
            view = VIEW_GAMES;
        }
        fix_scroll();
        return;
    }

    System *s = &systems[cur_sys];
    if (view == VIEW_DETAIL) {
        if (key == SW_BTN_B || key == SW_BTN_X)
            view = VIEW_GAMES;
        else if (key == SW_BTN_A || key == SW_BTN_START)
            launch_game(&s->games[sel]);
        else if (key == SW_BTN_RIGHT && sel < s->count - 1)
            sel++;
        else if (key == SW_BTN_LEFT && sel > 0)
            sel--;
        fix_scroll();
        return;
    }

    // VIEW_GAMES
    int cols = mode == MODE_GRID ? GRID_COLS : 1;
    int page = mode == MODE_GRID ? GRID_COLS * GRID_ROWS : LIST_ROWS;
    if (key == SW_BTN_B) {
        view = VIEW_HOME;
        home_sel = cur_sys;
    }
    else if (key == SW_BTN_A || key == SW_BTN_START)
        launch_game(&s->games[sel]);
    else if (key == SW_BTN_X)
        view = VIEW_DETAIL;
    else if (key == SW_BTN_Y) {
        mode = mode == MODE_GRID ? MODE_LIST : MODE_GRID;
        scroll_row = 0;
    }
    else if (mode == MODE_GRID && key == SW_BTN_RIGHT && sel < s->count - 1)
        sel++;
    else if (mode == MODE_GRID && key == SW_BTN_LEFT && sel > 0)
        sel--;
    else if (key == SW_BTN_DOWN)
        sel = sel + cols < s->count ? sel + cols : (sel / cols < (s->count - 1) / cols ? s->count - 1 : sel);
    else if (key == SW_BTN_UP && sel >= cols)
        sel -= cols;
    else if (key == SW_BTN_R1 || (mode == MODE_LIST && key == SW_BTN_RIGHT))
        sel = sel + page < s->count ? sel + page : s->count - 1;
    else if (key == SW_BTN_L1 || (mode == MODE_LIST && key == SW_BTN_LEFT))
        sel = sel - page > 0 ? sel - page : 0;
    else if (key == SW_BTN_R2)
        jump_letter(s, 1);
    else if (key == SW_BTN_L2)
        jump_letter(s, -1);
    fix_scroll();
}

/* ---------- scripted input for tests ---------- */

static char *script_tokens = NULL;
static char *script_pos = NULL;
static Uint32 script_wait_until = 0;

static bool run_script_step(void)
{
    if (!script_pos)
        return false;
    if (SDL_GetTicks() < script_wait_until)
        return true;
    if (dirty)
        return true; // let all art load before the next step
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
        SDL_SaveBMP(screen, tok + 5);
    }
    else if (!strncmp(tok, "wait:", 5))
        script_wait_until = SDL_GetTicks() + atoi(tok + 5);
    else
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(tok, keys[i].name))
                on_key(keys[i].key);
    return true;
}

/* ---------- main ---------- */

int main(void)
{
    const char *env;
    if ((env = getenv("SDCARD")))
        copy_str(sdcard, sizeof(sdcard), env);
    if ((env = getenv("GAMELIB_FONT")))
        copy_str(font_path, sizeof(font_path), env);
    else
        snprintf(font_path, sizeof(font_path), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    if ((env = getenv("GAMELIB_SCRIPT"))) {
        script_tokens = strdup(env);
        script_pos = script_tokens;
    }

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 50);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!soup_open_fonts(font_path)) {
        fprintf(stderr, "gameLibrary: cannot open font %s\n", font_path);
        return 1;
    }

    // loading screen while the lists are read
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    draw_topbar("Spill", "");
    const char *loading = "Henter spill…";
    text(f24, loading, (SCREEN_W - text_w(f24, loading)) / 2, 210, C_SOFT);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);

    load_all();
    load_state();

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = true;
            else if (e.type == SDL_KEYDOWN && !script_pos)
                on_key(e.key.keysym.sym);
        }
        if (dirty || (*msg && SDL_GetTicks() < msg_until + 100))
            draw();
        run_script_step();
        SDL_Delay(dirty ? 1 : 16);
    }

    save_state();
    if (!launched) {
        // leaving on purpose: do not come back here after the next game
        remove("/tmp/soup_library_return");
    }
    free(script_tokens);
    TTF_Quit();
    SDL_Quit();
    return launched ? 0 : 1;
}

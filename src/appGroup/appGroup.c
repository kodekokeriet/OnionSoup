// appGroup: shows one app group from the Apps tab ("Media", "Settings")
// as a list in the same style as MainUI, and makes the group icon collage.
//
//   appGroup list <title> <members file> [-s <index>]
//       Shows the apps listed in <members file> (one App/ folder name per
//       line). Writes the chosen folder name to /tmp/app_group_choice and the
//       list position to /tmp/app_group_index. Exit codes:
//         0 = open the chosen app, 1 = back, 2 = move the chosen app (SELECT),
//         3 = "Add app" row chosen
//
//   appGroup icon <out.png> <icon1> [icon2] [icon3] [icon4]
//       Writes a 2x2 collage of the given icons, used as the group's icon.
//
// The group logic itself lives in .tmp_update/script/app_groups.sh.

#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_rotozoom.h>
#include <SDL/SDL_ttf.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "components/list.h"
#include "system/battery.h"
#include "system/display.h"
#include "system/keymap_sw.h"
#include "system/lang.h"
#include "system/settings.h"
#include "theme/background.h"
#include "theme/sound.h"
#include "theme/theme.h"
#include "utils/IMG_Save.h"
#include "utils/file.h"
#include "utils/json.h"
#include "utils/keystate.h"
#include "utils/log.h"
#include "utils/sdl_init.h"

#define FRAMES_PER_SECOND 60
#define MAX_APPS 100
#define ICON_MAX 80

#define APP_DIR "/mnt/SDCARD/App"
#define CHOICE_PATH "/tmp/app_group_choice"
#define INDEX_PATH "/tmp/app_group_index"

#define EXIT_OPEN 0
#define EXIT_BACK 1
#define EXIT_MOVE 2
#define EXIT_ADD 3

static bool quit = false;
static KeyState keystate[320] = {(KeyState)0};

static void sigHandler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        quit = true;
}

static const char *app_dir(void)
{
    const char *env = getenv("APP_GROUP_APPDIR"); // for testing off-device
    return env ? env : APP_DIR;
}

static SDL_Surface *load_icon(const char *path, int max)
{
    SDL_Surface *icon = IMG_Load(path);
    if (!icon)
        return NULL;
    if (icon->w > max || icon->h > max) {
        double scale = (double)max / (icon->w > icon->h ? icon->w : icon->h);
        SDL_Surface *scaled = zoomSurface(icon, scale, scale, SMOOTHING_ON);
        SDL_FreeSurface(icon);
        icon = scaled;
    }
    return icon;
}

// Reads config.json, or config.grouped.json when the app is hidden in a group
static cJSON *load_app_config(const char *name, char *out_dir)
{
    char path[STR_MAX * 2];
    snprintf(out_dir, STR_MAX * 2, "%s/%s", app_dir(), name);
    snprintf(path, sizeof(path), "%s/config.grouped.json", out_dir);
    if (!is_file(path))
        snprintf(path, sizeof(path), "%s/config.json", out_dir);
    if (!is_file(path))
        return NULL;
    return json_load(path);
}

static bool add_app(List *list, const char *name)
{
    char dir[STR_MAX * 2], icon_rel[STR_MAX], icon_path[STR_MAX * 3];
    cJSON *config = load_app_config(name, dir);
    if (!config)
        return false;

    ListItem item = {.action_id = list->item_count};
    if (!json_getString(config, "label", item.label))
        strncpy(item.label, name, STR_MAX - 1);
    json_getString(config, "description", item.description);
    strncpy(item.payload, name, STR_MAX - 1);

    if (json_getString(config, "icon", icon_rel)) {
        if (icon_rel[0] == '/')
            snprintf(icon_path, sizeof(icon_path), "%s", icon_rel);
        else
            snprintf(icon_path, sizeof(icon_path), "%s/%s", dir, icon_rel);
        item.icon_ptr = load_icon(icon_path, ICON_MAX);
    }

    cJSON_Delete(config);
    list_addItem(list, item);
    return true;
}

static void write_result(const char *choice, int index)
{
    FILE *fp;
    file_put_sync(fp, CHOICE_PATH, "%s", choice);
    file_put_sync(fp, INDEX_PATH, "%d", index);
}

static int run_list(const char *title, const char *members_path, int selected)
{
    List list = list_create(MAX_APPS + 1, LIST_LARGE);

    FILE *fp = fopen(members_path, "r");
    if (fp) {
        char line[STR_MAX];
        while (list.item_count < MAX_APPS && fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) > 0)
                add_app(&list, line);
        }
        fclose(fp);
    }

    // Last row: pull an app from the top level of the Apps tab into this group
    ListItem add = {.action_id = -1};
    strcpy(add.label, "+ Add app");
    strcpy(add.description, "Move an app here from the Apps tab");
    char add_icon[STR_MAX * 2];
    snprintf(add_icon, sizeof(add_icon), "%s/../.tmp_update/res/app_group_add.png", app_dir());
    add.icon_ptr = load_icon(add_icon, ICON_MAX);
    list_addItem(&list, add);

    if (selected >= list.item_count)
        selected = list.item_count - 1;
    if (selected > 0)
        list_scrollTo(&list, selected);

    int battery_percentage = battery_getPercentage();
    bool list_changed = true, header_changed = true, footer_changed = true,
         battery_changed = true, key_changed = false;
    SDLKey changed_key = SDLK_UNKNOWN;
    int result = EXIT_BACK;

    uint32_t acc_ticks = 0, last_ticks = SDL_GetTicks(),
             time_step = 1000 / FRAMES_PER_SECOND;

    while (!quit) {
        uint32_t ticks = SDL_GetTicks();
        acc_ticks += ticks - last_ticks;
        last_ticks = ticks;

        if (updateKeystate(keystate, &quit, true, &changed_key)) {
            if (keystate[SW_BTN_DOWN] >= PRESSED) {
                key_changed = list_keyDown(&list, keystate[SW_BTN_DOWN] == REPEATING);
                list_changed = true;
                footer_changed = true;
            }
            else if (keystate[SW_BTN_UP] >= PRESSED) {
                key_changed = list_keyUp(&list, keystate[SW_BTN_UP] == REPEATING);
                list_changed = true;
                footer_changed = true;
            }
            else if (keystate[SW_BTN_RIGHT] >= PRESSED || keystate[SW_BTN_R1] >= PRESSED) {
                // page down, like MainUI
                int pos = list.active_pos + list.scroll_height;
                list_scrollTo(&list, pos < list.item_count ? pos : list.item_count - 1);
                key_changed = list_changed = footer_changed = true;
            }
            else if (keystate[SW_BTN_LEFT] >= PRESSED || keystate[SW_BTN_L1] >= PRESSED) {
                int pos = list.active_pos - list.scroll_height;
                list_scrollTo(&list, pos > 0 ? pos : 0);
                key_changed = list_changed = footer_changed = true;
            }

            ListItem *cur = list_currentItem(&list);
            bool is_add = cur->action_id == -1;

            if (changed_key == SW_BTN_A && keystate[SW_BTN_A] == RELEASED) {
                write_result(is_add ? "" : cur->payload, list.active_pos);
                result = is_add ? EXIT_ADD : EXIT_OPEN;
                quit = true;
            }
            else if (changed_key == SW_BTN_SELECT && keystate[SW_BTN_SELECT] == PRESSED && !is_add) {
                write_result(cur->payload, list.active_pos);
                result = EXIT_MOVE;
                quit = true;
            }
            else if (changed_key == SW_BTN_B && keystate[SW_BTN_B] == PRESSED) {
                write_result("", list.active_pos);
                result = EXIT_BACK;
                quit = true;
            }
        }

        if (key_changed || quit) {
            sound_change();
            key_changed = false;
        }
        if (quit)
            break;

        if (battery_hasChanged(ticks, &battery_percentage))
            battery_changed = true;

        if (acc_ticks >= time_step) {
            if (header_changed || battery_changed)
                theme_renderHeader(screen, title, false);
            if (list_changed)
                theme_renderList(screen, &list);
            if (footer_changed) {
                theme_renderFooter(screen);
                theme_renderStandardHint(screen, "Open", "Back    SELECT  Move");
            }
            if (footer_changed || list_changed)
                theme_renderFooterStatus(screen, list.active_pos + 1, list.item_count);
            if (header_changed || battery_changed)
                theme_renderHeaderBattery(screen, battery_getPercentage());

            header_changed = list_changed = footer_changed = battery_changed = false;

            SDL_BlitSurface(screen, NULL, video, NULL);
            SDL_Flip(video);
            acc_ticks -= time_step;
        }
    }

    list_free(&list);
    return result;
}

// 2x2 collage on a transparent square, sized like a regular app icon
static int make_icon(const char *out_path, int count, char **icons)
{
    SDL_Init(SDL_INIT_VIDEO);
    int size = 74;
    SDL_Surface *first = IMG_Load(icons[0]);
    if (first) {
        size = first->w > ICON_MAX ? ICON_MAX : first->w;
        SDL_FreeSurface(first);
    }

    SDL_Surface *out = SDL_CreateRGBSurface(SDL_SWSURFACE | SDL_SRCALPHA, size, size, 32,
                                            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    SDL_FillRect(out, NULL, SDL_MapRGBA(out->format, 0, 0, 0, 0));

    // folder look: rounded square with an orange edge, icons inside
    int radius = size / 5, edge = size / 25 + 1;
    Uint32 c_edge = SDL_MapRGBA(out->format, 255, 157, 90, 255);
    Uint32 c_fill = SDL_MapRGBA(out->format, 58, 51, 45, 235);
    SDL_LockSurface(out);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            // distance outside the rounded corner, 0 inside the straight parts
            int dx = x < radius ? radius - x : (x >= size - radius ? x - (size - radius - 1) : 0);
            int dy = y < radius ? radius - y : (y >= size - radius ? y - (size - radius - 1) : 0);
            int d2 = dx * dx + dy * dy;
            if (d2 > radius * radius)
                continue;
            int inner = radius - edge;
            bool on_edge = x < edge || y < edge || x >= size - edge || y >= size - edge ||
                           (dx && dy && d2 > inner * inner);
            ((Uint32 *)out->pixels)[y * out->pitch / 4 + x] = on_edge ? c_edge : c_fill;
        }
    }
    SDL_UnlockSurface(out);

    int pad = edge + size / 12;
    int gap = size / 18, cell = (size - 2 * pad - gap) / 2;
    for (int i = 0; i < count && i < 4; i++) {
        SDL_Surface *icon = load_icon(icons[i], cell);
        if (!icon)
            continue;
        SDL_Rect pos = {pad + (i % 2) * (cell + gap) + (cell - icon->w) / 2,
                        pad + (i / 2) * (cell + gap) + (cell - icon->h) / 2};
        // alpha-blend onto the folder background
        SDL_SetAlpha(icon, SDL_SRCALPHA, 255);
        SDL_BlitSurface(icon, NULL, out, &pos);
        SDL_FreeSurface(icon);
    }

    IMG_Save(out, (char *)out_path);
    SDL_FreeSurface(out);
    SDL_Quit();
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 4 && strcmp(argv[1], "icon") == 0)
        return make_icon(argv[2], argc - 3, &argv[3]);

    if (argc < 4 || strcmp(argv[1], "list") != 0) {
        fprintf(stderr, "usage: appGroup list <title> <members> [-s index]\n"
                        "       appGroup icon <out.png> <icon>...\n");
        return 64;
    }

    int selected = 0;
    if (argc >= 6 && strcmp(argv[4], "-s") == 0)
        selected = atoi(argv[5]);

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    SDL_InitDefault();
    settings_load();
    lang_load();

    int rc = run_list(argv[2], argv[3], selected);

    SDL_FillRect(video, NULL, 0);
    SDL_Flip(video);

    lang_free();
    resources_free();
    SDL_FreeSurface(screen);
    SDL_FreeSurface(video);
#ifndef PLATFORM_MIYOOMINI
    msleep(200);
#endif
    SDL_Quit();
    return rc;
}

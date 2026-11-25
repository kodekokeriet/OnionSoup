// OnionSoup Jellyfin client for Miyoo Mini+
//
// - Finds Jellyfin servers on the local network (UDP discovery, port 7359)
// - Signs in with Quick Connect (a 6-digit code approved from another device),
//   or with user name and password typed on the on-screen keyboard
// - Browses libraries as poster grids and track/episode lists
// - Plays music in the background with ffplay, and video by handing over to
//   ffplay (launch.sh runs it and starts this app again afterwards)
//
// Server, user and token are saved in Saves/CurrentProfile/config/jellyfin.json.
// No server address is built in.
//
// Environment overrides for testing on a PC:
//   SDCARD, JF_FONT, JF_CURL (curl binary), JF_PLAYER (audio player command),
//   JF_KEYBOARD_INPUT (answers the on-screen keyboard "types", split by '|'),
//   JF_SCRIPT (scripted input, e.g. "D A wait:500 shot:out.bmp")

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "soup/soup_ui.h"
#include "system/keymap_sw.h"

#ifdef PLATFORM_MIYOOMINI
#include "components/kbinput_wrapper.h"
#else
static const char *launch_keyboard(const char *initial, const char *title)
{
    // Host stub: JF_KEYBOARD_INPUT holds answers separated by '|'
    (void)title;
    static char buf[256];
    static int n = 0;
    const char *t = getenv("JF_KEYBOARD_INPUT");
    if (!t)
        return initial;
    for (int i = 0; i < n && t; i++) {
        t = strchr(t, '|');
        if (t)
            t++;
    }
    n++;
    if (!t)
        return initial;
    size_t l = strcspn(t, "|");
    if (l >= sizeof(buf))
        l = sizeof(buf) - 1;
    memcpy(buf, t, l);
    buf[l] = '\0';
    return buf;
}
#endif

#define APP_VERSION "0.1.0"
#define MAX_ITEMS 400
#define MAX_SERVERS 8
#define MAX_DEPTH 6
#define EXIT_PLAY_VIDEO 10

#define CARD_W 100
#define CARD_H 140
#define DETAIL_W 180
#define DETAIL_H 253

typedef struct {
    char id[40];
    char name[160];
    char type[24]; // Movie, Series, Season, Episode, MusicAlbum, Audio, Folder, CollectionFolder ...
    char overview[600];
    char sub[96]; // year, artist or episode number
    long runtime_ticks;
    bool is_folder;
    bool played;
    int img_state; // 0 none, 1 loading, 2 loaded, 3 failed
    SDL_Surface *img_card, *img_detail;
} Item;

typedef struct {
    char id[40];
    char name[96];
    char collection[24]; // movies, tvshows, music, ...
} View;

typedef struct {
    char parent_id[40];
    char title[160];
    Item *items;
    int count;
    int sel, scroll;
    bool list_mode; // rows (tracks, episodes) instead of poster grid
} Level;

typedef struct {
    char name[96];
    char address[160];
} Server;

typedef enum { S_DISCOVER,
               S_LOGIN,
               S_HOME,
               S_DETAIL } Screen;

// configuration
static char sdcard[256], workdir[300], config_path[400], font_path[300], curl_bin[300];
static char server_url[200] = "", user_id[64] = "", token[96] = "", user_name[96] = "", device_id[40] = "";

// state
static Screen scr = S_DISCOVER;
static bool quit = false;
static Server servers[MAX_SERVERS];
static int server_count = 0, server_sel = 0;
static int disc_sock = -1;
static Uint32 disc_until = 0;

static char qc_secret[128] = "", qc_code[16] = "";
static Uint32 qc_next_poll = 0;
static char login_error[200] = "";

static View views[8];
static int view_count = 0, view_sel = 0;
static Level stack[MAX_DEPTH];
static int depth = 0;
static Item *detail_item = NULL;

static char message[200] = "";
static Uint32 message_until = 0;

// image download (one at a time, in the background)
static pid_t img_pid = 0;
static Item *img_item = NULL;

// music
static pid_t music_pid = 0;
static bool music_paused = false;
static Level *music_level = NULL;
static int music_index = -1;
static char music_title[160] = "", music_artist[96] = "";

/* ---------- small helpers ---------- */

static void show_message(const char *t, Uint32 ms)
{
    copy_str(message, sizeof(message), t);
    message_until = SDL_GetTicks() + ms;
}

// Value for the Authorization header. Jellyfin 10.11 can turn off the legacy
// X-Emby-Authorization header and api_key parameter, so we send both styles.
static void auth_value(char *out, size_t n)
{
    snprintf(out, n,
             "MediaBrowser Client=\"OnionSoup\", Device=\"Miyoo Mini+\", "
             "DeviceId=\"%s\", Version=\"%s\"%s%s%s",
             device_id, APP_VERSION, *token ? ", Token=\"" : "", token, *token ? "\"" : "");
}

static void mkdir_p(const char *path)
{
    char tmp[400];
    copy_str(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* ---------- HTTP via curl ---------- */

static int last_http = 0;

// Append a line to the app log. Never log passwords or tokens.
static void jf_log(const char *fmt, ...)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/jellyfin.log", workdir);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Remove leading/trailing whitespace and newlines (from the keyboard).
static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t'))
        s[--l] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (i)
        memmove(s, s + i, l - i + 1);
}

// Run curl with the given arguments and return its stdout (malloc'd), or NULL.
static char *curl_capture(const char *method, const char *path, const char *body, int timeout_s)
{
    char url[600], val[360], auth[400], legacy[420], tmo[16];
    snprintf(url, sizeof(url), "%s%s", server_url, path);
    auth_value(val, sizeof(val));
    snprintf(auth, sizeof(auth), "Authorization: %s", val);
    snprintf(legacy, sizeof(legacy), "X-Emby-Authorization: %s", val);
    snprintf(tmo, sizeof(tmo), "%d", timeout_s);

    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, 2);
        const char *argv[20];
        int a = 0;
        argv[a++] = curl_bin;
        argv[a++] = "-s";
        argv[a++] = "-k";
        argv[a++] = "-w";
        argv[a++] = "\n%{http_code}";
        argv[a++] = "-m";
        argv[a++] = tmo;
        argv[a++] = "-X";
        argv[a++] = method;
        argv[a++] = "-H";
        argv[a++] = auth;
        argv[a++] = "-H";
        argv[a++] = legacy;
        argv[a++] = "-H";
        argv[a++] = "Content-Type: application/json";
        if (body) {
            argv[a++] = "--data-binary";
            argv[a++] = body;
        }
        argv[a++] = url;
        argv[a] = NULL;
        execvp(curl_bin, (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    ssize_t r;
    while (buf && (r = read(fds[0], buf + len, cap - len - 1)) > 0) {
        len += r;
        if (cap - len < 4096) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!buf)
        return NULL;
    buf[len] = '\0';
    // The last line is the HTTP status code added by -w
    last_http = 0;
    char *nl = strrchr(buf, '\n');
    if (nl) {
        last_http = atoi(nl + 1);
        *nl = '\0';
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code != 0 || last_http < 200 || last_http >= 300) {
        jf_log("%s %s -> curl %d, http %d", method, path, code, last_http);
        free(buf);
        return NULL;
    }
    return buf;
}

static cJSON *api_json(const char *method, const char *path, const char *body)
{
    char *res = curl_capture(method, path, body, 15);
    if (!res)
        return NULL;
    cJSON *j = cJSON_Parse(res);
    free(res);
    return j;
}

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* ---------- config ---------- */

static void save_config(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "server", server_url);
    cJSON_AddStringToObject(j, "user_id", user_id);
    cJSON_AddStringToObject(j, "user_name", user_name);
    cJSON_AddStringToObject(j, "token", token);
    cJSON_AddStringToObject(j, "device_id", device_id);
    char *s = cJSON_Print(j);
    FILE *fp = fopen(config_path, "w");
    if (fp) {
        fputs(s, fp);
        fclose(fp);
    }
    free(s);
    cJSON_Delete(j);
}

static void load_config(void)
{
    char *data = read_all(config_path);
    if (data) {
        cJSON *j = cJSON_Parse(data);
        if (j) {
            copy_str(server_url, sizeof(server_url), jstr(j, "server"));
            copy_str(user_id, sizeof(user_id), jstr(j, "user_id"));
            copy_str(user_name, sizeof(user_name), jstr(j, "user_name"));
            copy_str(token, sizeof(token), jstr(j, "token"));
            copy_str(device_id, sizeof(device_id), jstr(j, "device_id"));
            cJSON_Delete(j);
        }
        free(data);
    }
    if (!*device_id) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (int i = 0; i < 16; i++)
            device_id[i] = "0123456789abcdef"[rand() % 16];
        device_id[16] = '\0';
        save_config();
    }
}

static void sign_out(void)
{
    token[0] = user_id[0] = user_name[0] = '\0';
    save_config();
}

/* ---------- discovery ---------- */

static void discovery_start(void)
{
    server_count = 0;
    server_sel = 0;
    if (disc_sock >= 0)
        close(disc_sock);
    disc_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (disc_sock < 0)
        return;
    int yes = 1;
    setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    fcntl(disc_sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(7359);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char *msg = "Who is JellyfinServer?";
    sendto(disc_sock, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to));
    disc_until = SDL_GetTicks() + 3000;
}

static void discovery_poll(void)
{
    if (disc_sock < 0)
        return;
    char buf[2048];
    ssize_t n;
    while ((n = recv(disc_sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        cJSON *j = cJSON_Parse(buf);
        if (!j)
            continue;
        const char *addr = jstr(j, "Address");
        bool dup = false;
        for (int i = 0; i < server_count; i++)
            if (!strcmp(servers[i].address, addr))
                dup = true;
        if (*addr && !dup && server_count < MAX_SERVERS) {
            copy_str(servers[server_count].address, sizeof(servers[0].address), addr);
            copy_str(servers[server_count].name, sizeof(servers[0].name), jstr(j, "Name"));
            server_count++;
        }
        cJSON_Delete(j);
    }
    if (SDL_GetTicks() > disc_until) {
        close(disc_sock);
        disc_sock = -1;
    }
}

/* ---------- login ---------- */

static bool check_server(void)
{
    cJSON *j = api_json("GET", "/System/Info/Public", NULL);
    bool ok = j && *jstr(j, "Id");
    cJSON_Delete(j);
    return ok;
}

static bool finish_auth(cJSON *res)
{
    if (!res)
        return false;
    cJSON *user = cJSON_GetObjectItem(res, "User");
    copy_str(token, sizeof(token), jstr(res, "AccessToken"));
    copy_str(user_id, sizeof(user_id), jstr(user, "Id"));
    copy_str(user_name, sizeof(user_name), jstr(user, "Name"));
    cJSON_Delete(res);
    if (!*token)
        return false;
    save_config();
    return true;
}

static void quick_connect_start(void)
{
    qc_secret[0] = qc_code[0] = login_error[0] = '\0';
    token[0] = '\0';
    cJSON *j = api_json("POST", "/QuickConnect/Initiate", NULL);
    if (!j) {
        copy_str(login_error, sizeof(login_error), "Quick Connect er slått av på serveren. Trykk Y for passord.");
        return;
    }
    copy_str(qc_secret, sizeof(qc_secret), jstr(j, "Secret"));
    copy_str(qc_code, sizeof(qc_code), jstr(j, "Code"));
    cJSON_Delete(j);
    if (getenv("JF_DEBUG"))
        fprintf(stderr, "quickconnect code %s\n", qc_code);
    qc_next_poll = SDL_GetTicks() + 2000;
}

static void load_views(void);

static void quick_connect_poll(void)
{
    if (!*qc_secret || SDL_GetTicks() < qc_next_poll)
        return;
    qc_next_poll = SDL_GetTicks() + 2000;
    char path[256];
    snprintf(path, sizeof(path), "/QuickConnect/Connect?secret=%s", qc_secret);
    cJSON *j = api_json("GET", path, NULL);
    bool ok = j && cJSON_IsTrue(cJSON_GetObjectItem(j, "Authenticated"));
    cJSON_Delete(j);
    if (!ok)
        return;
    char body[256];
    snprintf(body, sizeof(body), "{\"Secret\":\"%s\"}", qc_secret);
    if (finish_auth(api_json("POST", "/Users/AuthenticateWithQuickConnect", body))) {
        qc_secret[0] = '\0';
        load_views();
    }
}

static void password_login(void)
{
    char user[96], pass[128];
    copy_str(user, sizeof(user), launch_keyboard(user_name, "Brukernavn"));
    trim(user);
    if (!*user)
        return;
    copy_str(pass, sizeof(pass), launch_keyboard("", "Passord"));
    // Only the password is not trimmed of inner spaces; strip a stray newline.
    size_t pl = strlen(pass);
    while (pl && (pass[pl - 1] == '\n' || pass[pl - 1] == '\r'))
        pass[--pl] = '\0';
    jf_log("password login: user length %d, password length %d", (int)strlen(user), (int)pl);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "Username", user);
    cJSON_AddStringToObject(b, "Pw", pass);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    token[0] = '\0';
    bool ok = finish_auth(api_json("POST", "/Users/AuthenticateByName", body));
    free(body);
    if (ok)
        load_views();
    else if (last_http == 401)
        copy_str(login_error, sizeof(login_error), "Feil brukernavn eller passord.");
    else if (last_http == 0)
        copy_str(login_error, sizeof(login_error), "Fikk ikke kontakt med serveren.");
    else
        snprintf(login_error, sizeof(login_error), "Serveren svarte med feil %d.", last_http);
}

static void choose_server(const char *address)
{
    char addr[200];
    copy_str(addr, sizeof(addr), address);
    size_t l = strlen(addr);
    while (l && addr[l - 1] == '/')
        addr[--l] = '\0';
    if (strncmp(addr, "http://", 7) && strncmp(addr, "https://", 8)) {
        char tmp[220];
        snprintf(tmp, sizeof(tmp), "http://%s", addr);
        copy_str(addr, sizeof(addr), tmp);
    }
    copy_str(server_url, sizeof(server_url), addr);
    if (!check_server()) {
        show_message("Fant ingen Jellyfin-server på den adressen", 3500);
        return;
    }
    sign_out();
    scr = S_LOGIN;
    quick_connect_start();
}

/* ---------- library ---------- */

static void free_level(Level *lv)
{
    for (int i = 0; i < lv->count; i++) {
        if (lv->items[i].img_card)
            SDL_FreeSurface(lv->items[i].img_card);
        if (lv->items[i].img_detail)
            SDL_FreeSurface(lv->items[i].img_detail);
    }
    free(lv->items);
    memset(lv, 0, sizeof(*lv));
}

static void fill_item(Item *it, cJSON *o)
{
    memset(it, 0, sizeof(*it));
    copy_str(it->id, sizeof(it->id), jstr(o, "Id"));
    copy_str(it->name, sizeof(it->name), jstr(o, "Name"));
    copy_str(it->type, sizeof(it->type), jstr(o, "Type"));
    copy_str(it->overview, sizeof(it->overview), jstr(o, "Overview"));
    it->is_folder = cJSON_IsTrue(cJSON_GetObjectItem(o, "IsFolder"));
    cJSON *ticks = cJSON_GetObjectItem(o, "RunTimeTicks");
    it->runtime_ticks = cJSON_IsNumber(ticks) ? (long)(ticks->valuedouble / 10000000.0) : 0; // seconds
    cJSON *ud = cJSON_GetObjectItem(o, "UserData");
    it->played = ud && cJSON_IsTrue(cJSON_GetObjectItem(ud, "Played"));
    cJSON *year = cJSON_GetObjectItem(o, "ProductionYear");
    cJSON *idx = cJSON_GetObjectItem(o, "IndexNumber");
    if (!strcmp(it->type, "Audio")) {
        const char *artist = jstr(o, "AlbumArtist");
        if (cJSON_IsNumber(idx))
            snprintf(it->sub, sizeof(it->sub), "%d", idx->valueint);
        (void)artist;
    }
    else if (!strcmp(it->type, "Episode") && cJSON_IsNumber(idx))
        snprintf(it->sub, sizeof(it->sub), "Episode %d", idx->valueint);
    else if (!strcmp(it->type, "MusicAlbum"))
        copy_str(it->sub, sizeof(it->sub), jstr(o, "AlbumArtist"));
    else if (cJSON_IsNumber(year))
        snprintf(it->sub, sizeof(it->sub), "%d", year->valueint);
}

// Load the children of parent_id into a new level on the stack
static bool push_level(const char *parent_id, const char *title, const char *collection)
{
    if (depth >= MAX_DEPTH)
        return false;
    char path[600], filter[160] = "";
    if (collection && !strcmp(collection, "movies"))
        copy_str(filter, sizeof(filter), "&Recursive=true&IncludeItemTypes=Movie");
    else if (collection && !strcmp(collection, "tvshows"))
        copy_str(filter, sizeof(filter), "&Recursive=true&IncludeItemTypes=Series");
    else if (collection && !strcmp(collection, "music"))
        copy_str(filter, sizeof(filter), "&Recursive=true&IncludeItemTypes=MusicAlbum");
    snprintf(path, sizeof(path),
             "/Users/%s/Items?ParentId=%s&SortBy=ParentIndexNumber,IndexNumber,SortName"
             "&Fields=Overview,ProductionYear&EnableImageTypes=Primary&Limit=%d%s",
             user_id, parent_id, MAX_ITEMS, filter);
    cJSON *j = api_json("GET", path, NULL);
    if (!j) {
        show_message("Kunne ikke hente fra serveren", 3000);
        return false;
    }
    Level *lv = &stack[depth];
    memset(lv, 0, sizeof(*lv));
    copy_str(lv->parent_id, sizeof(lv->parent_id), parent_id);
    copy_str(lv->title, sizeof(lv->title), title);
    cJSON *arr = cJSON_GetObjectItem(j, "Items"), *o;
    int n = cJSON_GetArraySize(arr);
    if (n > MAX_ITEMS)
        n = MAX_ITEMS;
    lv->items = calloc(n ? n : 1, sizeof(Item));
    cJSON_ArrayForEach(o, arr)
    {
        if (lv->count >= n)
            break;
        fill_item(&lv->items[lv->count++], o);
    }
    cJSON_Delete(j);
    lv->list_mode = lv->count > 0 && (!strcmp(lv->items[0].type, "Audio") || !strcmp(lv->items[0].type, "Episode"));
    depth++;
    return true;
}

static void pop_level(void)
{
    if (depth <= 1)
        return;
    depth--;
    if (music_level == &stack[depth]) {
        // keep music playing, but forget the list it came from
        music_level = NULL;
    }
    free_level(&stack[depth]);
}

static void open_view(int idx)
{
    while (depth > 0) {
        depth--;
        if (music_level == &stack[depth])
            music_level = NULL;
        free_level(&stack[depth]);
    }
    if (idx < 0 || idx >= view_count)
        return;
    view_sel = idx;
    push_level(views[idx].id, views[idx].name, views[idx].collection);
}

static void load_views(void)
{
    char path[200];
    snprintf(path, sizeof(path), "/Users/%s/Views", user_id);
    cJSON *j = api_json("GET", path, NULL);
    if (!j) {
        // token expired or server gone: sign in again
        scr = S_LOGIN;
        quick_connect_start();
        return;
    }
    view_count = 0;
    cJSON *arr = cJSON_GetObjectItem(j, "Items"), *o;
    cJSON_ArrayForEach(o, arr)
    {
        if (view_count >= 8)
            break;
        const char *coll = jstr(o, "CollectionType");
        if (!strcmp(coll, "playlists") || !strcmp(coll, "livetv") || !strcmp(coll, "boxsets"))
            continue;
        View *v = &views[view_count++];
        copy_str(v->id, sizeof(v->id), jstr(o, "Id"));
        copy_str(v->name, sizeof(v->name), jstr(o, "Name"));
        copy_str(v->collection, sizeof(v->collection), coll);
    }
    cJSON_Delete(j);
    scr = S_HOME;
    open_view(0);
}

/* ---------- images ---------- */

static void img_path(Item *it, char *out, size_t n)
{
    snprintf(out, n, "%s/img/%s.png", workdir, it->id);
}

static void load_images(Item *it)
{
    char p[400];
    img_path(it, p, sizeof(p));
    SDL_Surface *img = IMG_Load(p);
    if (!img) {
        it->img_state = 3;
        return;
    }
    // centre-crop to the poster shape so square album covers are not stretched
    int cw = img->w, ch = img->h;
    if (cw * CARD_H > ch * CARD_W)
        cw = ch * CARD_W / CARD_H;
    else
        ch = cw * CARD_H / CARD_W;
    SDL_Surface *crop = SDL_CreateRGBSurface(SDL_SWSURFACE, cw, ch, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (crop) {
        SDL_Rect src = {(img->w - cw) / 2, (img->h - ch) / 2, cw, ch};
        SDL_SetAlpha(img, 0, 255);
        SDL_BlitSurface(img, &src, crop, NULL);
        SDL_FreeSurface(img);
        img = crop;
    }
    it->img_card = scale_rounded(img, CARD_W, CARD_H, 8);
    it->img_detail = scale_rounded(img, DETAIL_W, DETAIL_H, 12);
    SDL_FreeSurface(img);
    it->img_state = 2;
}

static void request_image(Item *it)
{
    if (it->img_state != 0)
        return;
    char p[400];
    img_path(it, p, sizeof(p));
    if (access(p, R_OK) == 0) {
        load_images(it);
        return;
    }
    if (img_pid > 0)
        return;
    char url[600];
    snprintf(url, sizeof(url), "%s/Items/%s/Images/Primary?fillWidth=180&fillHeight=253&format=Png&quality=85&api_key=%s&ApiKey=%s",
             server_url, it->id, token, token);
    char auth[400], val[360];
    auth_value(val, sizeof(val));
    snprintf(auth, sizeof(auth), "Authorization: %s", val);
    it->img_state = 1;
    img_item = it;
    img_pid = fork();
    if (img_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        char tmp[420];
        snprintf(tmp, sizeof(tmp), "%s.tmp", p);
        pid_t c = fork();
        if (c == 0) {
            execlp(curl_bin, curl_bin, "-s", "-k", "-f", "-m", "20", "-H", auth, "-o", tmp, url, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        waitpid(c, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && rename(tmp, p) == 0)
            _exit(0);
        remove(tmp);
        _exit(1);
    }
}

static bool item_alive(Item *it)
{
    for (int d = 0; d < depth; d++)
        if (it >= stack[d].items && it < stack[d].items + stack[d].count)
            return true;
    return false;
}

static void poll_images(void)
{
    if (img_pid <= 0)
        return;
    int st;
    if (waitpid(img_pid, &st, WNOHANG) == img_pid) {
        img_pid = 0;
        if (img_item && item_alive(img_item)) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                load_images(img_item);
            else
                img_item->img_state = 3;
        }
        img_item = NULL;
    }
}

/* ---------- playback ---------- */

static void music_stop(void)
{
    if (music_pid > 0) {
        if (music_paused)
            kill(music_pid, SIGCONT);
        kill(music_pid, SIGTERM);
        waitpid(music_pid, NULL, 0);
    }
    music_pid = 0;
    music_paused = false;
}

static void music_play(Level *lv, int index)
{
    music_stop();
    if (!lv || index < 0 || index >= lv->count)
        return;
    Item *it = &lv->items[index];
    music_level = lv;
    music_index = index;
    copy_str(music_title, sizeof(music_title), it->name);
    copy_str(music_artist, sizeof(music_artist), lv->title);
    char url[600];
    snprintf(url, sizeof(url), "%s/Audio/%s/stream.mp3?api_key=%s&ApiKey=%s&AudioCodec=mp3&AudioBitrate=192000&Static=false&DeviceId=%s",
             server_url, it->id, token, token, device_id);
    char headers[420], val[360];
    auth_value(val, sizeof(val));
    snprintf(headers, sizeof(headers), "Authorization: %s\r\n", val);
    const char *player = getenv("JF_PLAYER");
    music_pid = fork();
    if (music_pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        if (player)
            execl("/bin/sh", "sh", "-c", player, "player", url, (char *)NULL);
        else
            execl("./bin/ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-headers", headers, "-i", url,
                  (char *)NULL);
        _exit(127);
    }
}

static void poll_music(void)
{
    if (music_pid <= 0)
        return;
    int st;
    if (waitpid(music_pid, &st, WNOHANG) == music_pid) {
        music_pid = 0;
        // track finished: play the next one in the same list
        if (music_level && music_index + 1 < music_level->count)
            music_play(music_level, music_index + 1);
        else
            music_index = -1;
    }
}

static void play_video(Item *it)
{
    music_stop();
    char url[800];
    snprintf(url, sizeof(url),
             "%s/Videos/%s/stream.ts?api_key=%s&ApiKey=%s&VideoCodec=h264&AudioCodec=mp3&MaxWidth=640&MaxHeight=480"
             "&VideoBitrate=1000000&AudioBitrate=128000&AudioChannels=2&TranscodingMaxAudioChannels=2"
             "&h264-profile=baseline&h264-level=30&Static=false&MediaSourceId=%s&DeviceId=%s",
             server_url, it->id, token, token, it->id, device_id);
    FILE *fp = fopen("/tmp/jellyfin_headers", "w");
    if (fp) {
        char val[360];
        auth_value(val, sizeof(val));
        fprintf(fp, "Authorization: %s\r\n", val);
        fclose(fp);
    }
    fp = fopen("/tmp/jellyfin_play", "w");
    if (!fp) {
        show_message("Kunne ikke starte avspilling", 3000);
        return;
    }
    fprintf(fp, "%s\n", url);
    fclose(fp);
    // remember where we were, so the app can come back to the same place
    fp = fopen("/tmp/jellyfin_state", "w");
    if (fp) {
        fprintf(fp, "%d\n", view_sel);
        for (int d = 1; d < depth; d++)
            fprintf(fp, "%s\t%s\n", stack[d].parent_id, stack[d].title);
        fprintf(fp, "#detail\t%s\n", it->id);
        fclose(fp);
    }
    quit = true;
}

static int exit_code = 0;

static void restore_state(void)
{
    FILE *fp = fopen("/tmp/jellyfin_state", "r");
    if (!fp)
        return;
    char line[300];
    if (fgets(line, sizeof(line), fp)) {
        int v = atoi(line);
        if (v != view_sel && v < view_count)
            open_view(v);
    }
    char detail_id[40] = "";
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        if (!strcmp(line, "#detail"))
            copy_str(detail_id, sizeof(detail_id), tab + 1);
        else
            push_level(line, tab + 1, NULL);
    }
    fclose(fp);
    remove("/tmp/jellyfin_state");
    if (*detail_id && depth > 0) {
        Level *lv = &stack[depth - 1];
        for (int i = 0; i < lv->count; i++)
            if (!strcmp(lv->items[i].id, detail_id)) {
                lv->sel = i;
                detail_item = &lv->items[i];
                scr = S_DETAIL;
            }
    }
}

/* ---------- drawing ---------- */

static void draw_hint_bar(void)
{
    const char *k[4], *l[4];
    int n = 0;
#define HINT(a, b) (k[n] = (a), l[n] = (b), n++)
    if (scr == S_DISCOVER) {
        HINT("A", "Velg");
        HINT("Y", "Søk igjen");
        HINT("B", "Avslutt");
    }
    else if (scr == S_LOGIN) {
        HINT("Y", "Passord");
        HINT("X", "Ny kode");
        HINT("B", "Tilbake");
    }
    else if (scr == S_DETAIL) {
        HINT("A", "Spill av");
        HINT("B", "Tilbake");
    }
    else {
        Level *lv = depth ? &stack[depth - 1] : NULL;
        HINT("A", lv && lv->list_mode && lv->count && !strcmp(lv->items[0].type, "Audio") ? "Spill" : "Åpne");
        HINT("B", depth > 1 ? "Tilbake" : "Avslutt");
        if (depth == 1 && view_count > 1)
            HINT("L/R", "Bibliotek");
        if (music_pid > 0)
            HINT("Y", "Stopp musikk");
    }
    draw_hints(k, l, n);
#undef HINT
}

static void draw_topbar_tabs(void)
{
    soup_logo("Jellyfin");
    if (scr != S_HOME && scr != S_DETAIL)
        return;
    int tx = SCREEN_W - 16;
    for (int i = view_count - 1; i >= 0; i--)
        tx -= text_w(f17, views[i].name) + (i ? 26 : 0);
    if (tx < 200)
        tx = 200;
    for (int i = 0; i < view_count; i++) {
        int w = text_w(f17, views[i].name);
        if (tx + w > SCREEN_W - 6)
            break;
        if (i == view_sel) {
            fill_round(tx - 10, 12, w + 20, 28, 14, depth == 1 && scr == S_HOME ? C_ACCENT : C_PANEL);
            text(f17, views[i].name, tx, 15, depth == 1 && scr == S_HOME ? C_DARK : C_ACCENT);
        }
        else
            text(f17, views[i].name, tx, 15, C_GREY);
        tx += w + 26;
    }
}

static void draw_now_playing(void)
{
    if (music_pid <= 0)
        return;
    fill(0, BOTTOM_Y - 40, SCREEN_W, 40, C_PANEL);
    fill(0, BOTTOM_Y - 40, 4, 40, C_ACCENT);
    text(f13, music_paused ? "PAUSE" : "SPILLER", 16, BOTTOM_Y - 33, C_ACCENT);
    char line[300];
    snprintf(line, sizeof(line), "%s  ·  %s", music_title, music_artist);
    text_fit(f15, line, 90, BOTTOM_Y - 32, SCREEN_W - 210, C_WHITE);
    text(f13, "START pause", SCREEN_W - 104, BOTTOM_Y - 31, C_GREY);
}

static void draw_center_text(const char *big, const char *small, SDL_Color c)
{
    text(f24, big, (SCREEN_W - text_w(f24, big)) / 2, 190, c);
    if (small)
        text_wrap(f15, small, 90, 232, SCREEN_W - 180, 21, 4, C_GREY);
}

static void draw_discover(void)
{
    text(f24, "Velg server", 24, TOP_H + 16, C_WHITE);
    const char *sub = disc_sock >= 0 ? "Søker etter Jellyfin på nettverket …" : "Servere på nettverket ditt:";
    text(f15, sub, 24, TOP_H + 50, C_GREY);
    int rows = server_count + 1;
    for (int i = 0; i < rows; i++) {
        int y = TOP_H + 84 + i * 58;
        bool s = i == server_sel;
        fill_round(24, y, SCREEN_W - 48, 50, 12, s ? C_CARD : C_PANEL);
        if (s)
            fill_round(24, y, 6, 50, 3, C_ACCENT);
        if (i < server_count) {
            text_fit(f19, *servers[i].name ? servers[i].name : "Jellyfin", 44, y + 5, SCREEN_W - 100, C_WHITE);
            text_fit(f13, servers[i].address, 44, y + 29, SCREEN_W - 100, C_GREY);
        }
        else {
            text(f19, "Skriv inn adresse …", 44, y + 13, s ? C_ACCENT : C_SOFT);
        }
    }
    if (server_count == 0 && disc_sock < 0)
        text_wrap(f15, "Fant ingen servere. Sjekk at Miyoo-en er på samme WiFi som serveren, eller skriv inn adressen.", 24,
                  TOP_H + 160, SCREEN_W - 48, 21, 3, C_GREY);
}

static void draw_login(void)
{
    if (*qc_code) {
        text(f19, "Logg inn med Quick Connect", 24, TOP_H + 18, C_WHITE);
        // big code, grouped 3+3
        char code[16];
        snprintf(code, sizeof(code), "%.3s %.3s", qc_code, qc_code + 3);
        static TTF_Font *big = NULL;
        if (!big)
            big = TTF_OpenFont(font_path, 64);
        TTF_Font *f = big ? big : f34;
        int cw = text_w(f, code);
        fill_round((SCREEN_W - cw) / 2 - 30, TOP_H + 60, cw + 60, 100, 20, C_PANEL);
        text(f, code, (SCREEN_W - cw) / 2, TOP_H + 110 - TTF_FontHeight(f) / 2, C_ACCENT);
        static const char *steps[] = {"1. Åpne Jellyfin på mobilen eller PC-en",
                                      "2. Gå til profilen din og velg «Quick Connect»",
                                      "3. Skriv inn koden over"};
        for (int i = 0; i < 3; i++)
            text(f17, steps[i], 60, TOP_H + 186 + i * 30, C_SOFT);
        Uint32 t2 = SDL_GetTicks() / 400 % 4;
        char wait[40];
        snprintf(wait, sizeof(wait), "Venter på godkjenning%.*s", (int)t2, "...");
        if (*login_error)
            text_fit(f15, login_error, 60, TOP_H + 290, SCREEN_W - 120, C_RED);
        else
            text(f15, wait, 60, TOP_H + 290, C_GREY);
        text_fit(f13, server_url, 60, TOP_H + 316, SCREEN_W - 120, C_GREY);
    }
    else {
        draw_center_text("Logg inn", *login_error ? login_error : "Trykk Y for å logge inn med brukernavn og passord.",
                         C_WHITE);
    }
}

static void draw_card(Item *it, int x, int y, bool selected)
{
    request_image(it);
    if (selected)
        outline_round(x - 5, y - 5, CARD_W + 10, CARD_H + 10, 12, 3, C_ACCENT, C_BG);
    if (it->img_card) {
        SDL_Rect pos = {x, y};
        SDL_BlitSurface(it->img_card, NULL, screen, &pos);
    }
    else {
        fill_round(x, y, CARD_W, CARD_H, 8, C_CARD);
        char letter[2] = {(char)toupper((unsigned char)it->name[0]), 0};
        text(f24, letter, x + (CARD_W - text_w(f24, letter)) / 2, y + CARD_H / 2 - 14, C_ACCENT);
    }
    if (it->played) {
        fill_round(x + CARD_W - 30, y + 5, 25, 18, 9, C_GREEN);
        text(f13, "OK", x + CARD_W - 26, y + 6, C_WHITE);
    }
    text_fit(f13, it->name, x, y + CARD_H + 6, CARD_W, selected ? C_WHITE : C_GREY);
}

static void draw_grid(Level *lv)
{
    int bottom = music_pid > 0 ? BOTTOM_Y - 40 : BOTTOM_Y;
    int rows_visible = bottom - (TOP_H + 42) >= 352 ? 2 : 1;
    int row = lv->sel / 5;
    if (row < lv->scroll)
        lv->scroll = row;
    if (row > lv->scroll + rows_visible - 1)
        lv->scroll = row - rows_visible + 1;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 5; c++) {
            int i = (lv->scroll + r) * 5 + c;
            if (i >= lv->count)
                break;
            int y = TOP_H + 42 + r * 176;
            if (y + CARD_H > bottom)
                break;
            draw_card(&lv->items[i], 24 + c * 120, y, i == lv->sel);
        }
}

static void format_duration(long s, char *out, size_t n)
{
    if (s >= 3600)
        snprintf(out, n, "%ld t %ld min", s / 3600, (s % 3600) / 60);
    else
        snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

static void draw_list(Level *lv)
{
    int bottom = music_pid > 0 ? BOTTOM_Y - 40 : BOTTOM_Y;
    int row_h = 44;
    int visible = (bottom - (TOP_H + 44)) / row_h;
    if (lv->sel < lv->scroll)
        lv->scroll = lv->sel;
    if (lv->sel >= lv->scroll + visible)
        lv->scroll = lv->sel - visible + 1;
    for (int k = 0; k < visible && lv->scroll + k < lv->count; k++) {
        int i = lv->scroll + k;
        Item *it = &lv->items[i];
        int y = TOP_H + 42 + k * row_h;
        bool s = i == lv->sel;
        bool playing = music_level == lv && music_index == i && music_pid > 0;
        if (s)
            fill_round(16, y, SCREEN_W - 32, row_h - 4, 10, C_CARD);
        char num[16];
        snprintf(num, sizeof(num), "%d", i + 1);
        text(f15, playing ? "▶" : num, 30, y + 10, playing ? C_ACCENT : C_GREY);
        text_fit(f17, it->name, 70, y + 8, SCREEN_W - 200, playing ? C_ACCENT : (s ? C_WHITE : C_SOFT));
        char dur[32];
        format_duration(it->runtime_ticks, dur, sizeof(dur));
        if (it->runtime_ticks)
            text(f15, dur, SCREEN_W - 40 - text_w(f15, dur), y + 10, C_GREY);
    }
}

static void draw_home(void)
{
    if (depth == 0) {
        draw_center_text("Ingen bibliotek", "Serveren har ingen bibliotek du har tilgang til.", C_WHITE);
        return;
    }
    Level *lv = &stack[depth - 1];
    text_fit(f19, lv->title, 24, TOP_H + 8, SCREEN_W - 120, C_WHITE);
    char count[16];
    snprintf(count, sizeof(count), "%d", lv->count);
    text(f15, count, SCREEN_W - 24 - text_w(f15, count), TOP_H + 12, C_GREY);
    if (lv->count == 0) {
        draw_center_text("Tomt", "Ingenting her ennå.", C_GREY);
        return;
    }
    if (lv->sel >= lv->count)
        lv->sel = lv->count - 1;
    if (lv->list_mode)
        draw_list(lv);
    else
        draw_grid(lv);
}

static void draw_detail(void)
{
    Item *it = detail_item;
    request_image(it);
    if (it->img_detail) {
        SDL_Rect pos = {24, TOP_H + 18};
        SDL_BlitSurface(it->img_detail, NULL, screen, &pos);
    }
    else
        fill_round(24, TOP_H + 18, DETAIL_W, DETAIL_H, 12, C_CARD);
    int x = 228, w = SCREEN_W - x - 24;
    int y = text_wrap(f34, it->name, x, TOP_H + 14, w, 38, 2, C_WHITE);
    char meta[160] = "", dur[32] = "";
    if (it->runtime_ticks)
        format_duration(it->runtime_ticks, dur, sizeof(dur));
    snprintf(meta, sizeof(meta), "%s%s%s", it->sub, *it->sub && *dur ? "  ·  " : "", dur);
    text_fit(f15, meta, x, y + 2, w, C_GREY);
    y += 34;
    y = text_wrap(f15, *it->overview ? it->overview : "Ingen beskrivelse.", x, y, w, 21, 6, C_SOFT);
    int by = TOP_H + 222;
    fill_round(x, by, text_w(f19, "Spill av") + 40, 36, 18, C_ACCENT);
    text(f19, "Spill av", x + 20, by + 6, C_DARK);
    if (it->played)
        text(f15, "Sett", x + text_w(f19, "Spill av") + 56, by + 9, C_GREEN);
    text_fit(f13, "Video omkodes av serveren til 640x480 for Miyoo.", 24, TOP_H + 300, SCREEN_W - 48, C_GREY);
}

static void draw(void)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    switch (scr) {
    case S_DISCOVER:
        draw_discover();
        break;
    case S_LOGIN:
        draw_login();
        break;
    case S_HOME:
        draw_home();
        break;
    case S_DETAIL:
        draw_detail();
        break;
    }
    draw_topbar_tabs();
    draw_now_playing();
    draw_hint_bar();
    if (*message && SDL_GetTicks() < message_until) {
        int w = text_w(f17, message) + 40;
        if (w > SCREEN_W - 40)
            w = SCREEN_W - 40;
        fill_round((SCREEN_W - w) / 2, BOTTOM_Y - 96, w, 40, 20, C_CARD);
        text_fit(f17, message, (SCREEN_W - w) / 2 + 20, BOTTOM_Y - 88, w - 40, C_WHITE);
    }
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

/* ---------- input ---------- */

static void open_item(Level *lv)
{
    Item *it = &lv->items[lv->sel];
    if (!strcmp(it->type, "Audio")) {
        music_play(lv, lv->sel);
    }
    else if (it->is_folder) {
        push_level(it->id, it->name, NULL);
    }
    else {
        detail_item = it;
        scr = S_DETAIL;
    }
}

static void on_key(SDLKey key)
{
    if (key == SW_BTN_START && music_pid > 0) {
        kill(music_pid, music_paused ? SIGCONT : SIGSTOP);
        music_paused = !music_paused;
        return;
    }
    switch (scr) {
    case S_DISCOVER:
        if (key == SW_BTN_UP && server_sel > 0)
            server_sel--;
        else if (key == SW_BTN_DOWN && server_sel < server_count)
            server_sel++;
        else if (key == SW_BTN_Y)
            discovery_start();
        else if (key == SW_BTN_B || key == SW_BTN_MENU)
            quit = true;
        else if (key == SW_BTN_A) {
            if (server_sel < server_count)
                choose_server(servers[server_sel].address);
            else {
                const char *addr = launch_keyboard(*server_url ? server_url : "http://", "Serveradresse");
                if (addr && *addr && strcmp(addr, "http://"))
                    choose_server(addr);
            }
        }
        break;
    case S_LOGIN:
        if (key == SW_BTN_B) {
            qc_secret[0] = qc_code[0] = '\0';
            scr = S_DISCOVER;
            discovery_start();
        }
        else if (key == SW_BTN_Y)
            password_login();
        else if (key == SW_BTN_X)
            quick_connect_start();
        break;
    case S_DETAIL:
        if (key == SW_BTN_B)
            scr = S_HOME;
        else if (key == SW_BTN_A)
            play_video(detail_item);
        break;
    case S_HOME: {
        if (key == SW_BTN_Y && music_pid > 0) {
            music_stop();
            music_index = -1;
            return;
        }
        if (key == SW_BTN_SELECT) { // sign out / change server
            music_stop();
            sign_out();
            scr = S_DISCOVER;
            discovery_start();
            return;
        }
        if (depth == 0) {
            if (key == SW_BTN_B)
                quit = true;
            return;
        }
        Level *lv = &stack[depth - 1];
        if (key == SW_BTN_B || key == SW_BTN_MENU) {
            if (depth > 1)
                pop_level();
            else
                quit = true;
        }
        else if ((key == SW_BTN_L1 || key == SW_BTN_R1) && depth == 1 && view_count > 1)
            open_view((view_sel + (key == SW_BTN_R1 ? 1 : view_count - 1)) % view_count);
        else if (lv->count == 0)
            return;
        else if (key == SW_BTN_A)
            open_item(lv);
        else if (lv->list_mode) {
            if (key == SW_BTN_DOWN && lv->sel < lv->count - 1)
                lv->sel++;
            else if (key == SW_BTN_UP && lv->sel > 0)
                lv->sel--;
            else if (key == SW_BTN_RIGHT)
                lv->sel = lv->sel + 8 < lv->count ? lv->sel + 8 : lv->count - 1;
            else if (key == SW_BTN_LEFT)
                lv->sel = lv->sel >= 8 ? lv->sel - 8 : 0;
        }
        else {
            if (key == SW_BTN_RIGHT && lv->sel < lv->count - 1)
                lv->sel++;
            else if (key == SW_BTN_LEFT && lv->sel > 0)
                lv->sel--;
            else if (key == SW_BTN_DOWN)
                lv->sel = lv->sel + 5 < lv->count ? lv->sel + 5 : lv->count - 1;
            else if (key == SW_BTN_UP && lv->sel >= 5)
                lv->sel -= 5;
        }
        break;
    }
    }
}

/* ---------- scripted input for tests ---------- */

static char *script = NULL, *spos = NULL;
static Uint32 swait = 0;

static void script_step(void)
{
    if (!spos || SDL_GetTicks() < swait || img_pid > 0)
        return;
    while (*spos == ' ')
        spos++;
    if (!*spos) {
        quit = true;
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
    } keys[] = {{"U", SW_BTN_UP}, {"D", SW_BTN_DOWN}, {"L", SW_BTN_LEFT}, {"R", SW_BTN_RIGHT}, {"A", SW_BTN_A}, {"B", SW_BTN_B}, {"X", SW_BTN_X}, {"Y", SW_BTN_Y}, {"L1", SW_BTN_L1}, {"R1", SW_BTN_R1}, {"START", SW_BTN_START}, {"SELECT", SW_BTN_SELECT}};
    if (!strncmp(tok, "shot:", 5)) {
        draw();
        SDL_SaveBMP(screen, tok + 5);
    }
    else if (!strncmp(tok, "wait:", 5))
        swait = SDL_GetTicks() + atoi(tok + 5);
    else
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(tok, keys[i].name))
                on_key(keys[i].key);
}

/* ---------- main ---------- */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *e;
    copy_str(sdcard, sizeof(sdcard), (e = getenv("SDCARD")) ? e : "/mnt/SDCARD");
    snprintf(workdir, sizeof(workdir), "%s/.tmp_update/jellyfin", sdcard);
    snprintf(config_path, sizeof(config_path), "%s/Saves/CurrentProfile/config/jellyfin.json", sdcard);
    if ((e = getenv("JF_FONT")))
        copy_str(font_path, sizeof(font_path), e);
    else
        snprintf(font_path, sizeof(font_path), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    copy_str(curl_bin, sizeof(curl_bin), (e = getenv("JF_CURL")) ? e : "curl");
    if ((e = getenv("JF_SCRIPT")))
        spos = script = strdup(e);
    remove("/tmp/jellyfin_play");
    char imgdir[400];
    snprintf(imgdir, sizeof(imgdir), "%s/img", workdir);
    mkdir_p(imgdir);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 60);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!soup_open_fonts(font_path)) {
        fprintf(stderr, "jellyfin: cannot open font %s\n", font_path);
        return 1;
    }

    load_config();
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    soup_logo("Jellyfin");
    text(f24, "Kobler til …", (SCREEN_W - text_w(f24, "Kobler til …")) / 2, 210, C_WHITE);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);

    if (*server_url && *token && check_server()) {
        load_views();
        if (scr == S_HOME)
            restore_state();
    }
    else if (*server_url && check_server()) {
        scr = S_LOGIN;
        quick_connect_start();
    }
    else {
        scr = S_DISCOVER;
        discovery_start();
    }

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = true;
            else if (ev.type == SDL_KEYDOWN && !spos)
                on_key(ev.key.keysym.sym);
        }
        script_step();
        discovery_poll();
        if (scr == S_LOGIN)
            quick_connect_poll();
        poll_images();
        poll_music();
        draw();
        SDL_Delay(30);
    }

    if (access("/tmp/jellyfin_play", F_OK) == 0)
        exit_code = EXIT_PLAY_VIDEO;
    music_stop();
    if (img_pid > 0)
        waitpid(img_pid, NULL, 0);
    free(script);
    TTF_Quit();
    SDL_Quit();
    return exit_code;
}

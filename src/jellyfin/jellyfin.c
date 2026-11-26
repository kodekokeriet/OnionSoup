// OnionSoup Jellyfin client for Miyoo Mini+
//
// - Finds Jellyfin servers on the local network (UDP discovery, port 7359)
// - Signs in with Quick Connect (a 6-digit code approved from another device),
//   or with user name and password typed on the on-screen keyboard
// - Browses libraries as poster grids and track/episode lists
// - Plays music in the background with ffplay, and video by handing over to
//   jfplayer (launch.sh runs it and starts this app again afterwards)
// - Downloads films, episodes and music (transcoded for the Miyoo) to
//   SDCARD/Jellyfin for offline use; without a server it shows only those
// - SELECT opens settings: servers, user, video quality, downloads
//
// Servers, users and tokens are saved in Saves/CurrentProfile/config/jellyfin.json.
// No server address is built in.
//
// Environment overrides for testing on a PC:
//   SDCARD, JF_FONT, JF_CURL (curl binary), JF_PLAYER (audio player command),
//   JF_KEYBOARD_INPUT (answers the on-screen keyboard "types", split by '|'),
//   JF_SCRIPT (scripted input, e.g. "D A wait:500 shot:out.bmp"),
//   JF_DOWNLOADS (download folder, default SDCARD/Jellyfin)

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

#define APP_VERSION "0.3.0"
#define MAX_ACCOUNTS 8
#define MAX_DL 600
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
    long resume_s;     // where playback stopped last time, in seconds
    char group[160];   // series or album name (for downloads)
    char group_id[40]; // series or album id
    int order;         // season * 1000 + episode/track number
    char local[240];   // downloaded file, for offline items
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
               S_DETAIL,
               S_SETTINGS,
               S_SERVERS } Screen;

typedef struct {
    char url[200], name[96], user_id[64], user_name[96], token[96];
} Account;

// rows of the settings screen
enum { SET_SERVER,
       SET_USER,
       SET_QUALITY,
       SET_DOWNLOADS,
       SET_DELETE_ALL,
       SET_ABOUT,
       SET_COUNT };

// configuration
static char sdcard[256], workdir[300], config_path[400], font_path[300], curl_bin[300];
static char server_url[200] = "", user_id[64] = "", token[96] = "", user_name[96] = "", device_id[40] = "";
static char server_name[96] = "";
static Account accounts[MAX_ACCOUNTS];
static int account_count = 0, account_sel = -1;
static int quality = 1; // index into qualities[]
static const struct {
    const char *name;
    int bitrate, max_w, max_h;
} qualities[] = {{"Lav (360p)", 500000, 480, 360}, {"Middels (480p)", 1000000, 640, 480}, {"Høy (480p)", 1600000, 640, 480}};
static bool offline = false;
static char dl_dir[300];
static Screen prev_scr = S_HOME; // where settings return to
static int settings_sel = 0, servers_sel = 0;
static Uint32 confirm_until = 0; // a delete waits for a second press until then
static char confirm_what[64] = "";

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

static void account_store(void)
{
    if (!*server_url)
        return;
    if (account_sel < 0 || account_sel >= account_count) {
        if (account_count >= MAX_ACCOUNTS)
            return;
        account_sel = account_count++;
    }
    Account *a = &accounts[account_sel];
    copy_str(a->url, sizeof(a->url), server_url);
    copy_str(a->name, sizeof(a->name), server_name);
    copy_str(a->user_id, sizeof(a->user_id), user_id);
    copy_str(a->user_name, sizeof(a->user_name), user_name);
    copy_str(a->token, sizeof(a->token), token);
}

static void account_load(int i)
{
    account_sel = i;
    Account *a = &accounts[i];
    copy_str(server_url, sizeof(server_url), a->url);
    copy_str(server_name, sizeof(server_name), a->name);
    copy_str(user_id, sizeof(user_id), a->user_id);
    copy_str(user_name, sizeof(user_name), a->user_name);
    copy_str(token, sizeof(token), a->token);
}

static void save_config(void)
{
    account_store();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "device_id", device_id);
    cJSON_AddNumberToObject(j, "quality", quality);
    cJSON_AddNumberToObject(j, "current", account_sel);
    cJSON *arr = cJSON_AddArrayToObject(j, "accounts");
    for (int i = 0; i < account_count; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "server", accounts[i].url);
        cJSON_AddStringToObject(a, "name", accounts[i].name);
        cJSON_AddStringToObject(a, "user_id", accounts[i].user_id);
        cJSON_AddStringToObject(a, "user_name", accounts[i].user_name);
        cJSON_AddStringToObject(a, "token", accounts[i].token);
        cJSON_AddItemToArray(arr, a);
    }
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
            copy_str(device_id, sizeof(device_id), jstr(j, "device_id"));
            cJSON *q = cJSON_GetObjectItem(j, "quality");
            if (cJSON_IsNumber(q) && q->valueint >= 0 && q->valueint < 3)
                quality = q->valueint;
            cJSON *arr = cJSON_GetObjectItem(j, "accounts"), *a;
            cJSON_ArrayForEach(a, arr)
            {
                if (account_count >= MAX_ACCOUNTS)
                    break;
                Account *x = &accounts[account_count++];
                copy_str(x->url, sizeof(x->url), jstr(a, "server"));
                copy_str(x->name, sizeof(x->name), jstr(a, "name"));
                copy_str(x->user_id, sizeof(x->user_id), jstr(a, "user_id"));
                copy_str(x->user_name, sizeof(x->user_name), jstr(a, "user_name"));
                copy_str(x->token, sizeof(x->token), jstr(a, "token"));
            }
            if (!arr && *jstr(j, "server")) { // config from version 0.2
                Account *x = &accounts[account_count++];
                copy_str(x->url, sizeof(x->url), jstr(j, "server"));
                copy_str(x->user_id, sizeof(x->user_id), jstr(j, "user_id"));
                copy_str(x->user_name, sizeof(x->user_name), jstr(j, "user_name"));
                copy_str(x->token, sizeof(x->token), jstr(j, "token"));
            }
            cJSON *cur = cJSON_GetObjectItem(j, "current");
            int c = cJSON_IsNumber(cur) ? cur->valueint : 0;
            if (account_count > 0)
                account_load(c >= 0 && c < account_count ? c : 0);
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
    token[0] = user_id[0] = '\0';
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
    // short timeout: without WiFi the app should go offline quickly
    char *res = curl_capture("GET", "/System/Info/Public", NULL, 5);
    cJSON *j = res ? cJSON_Parse(res) : NULL;
    free(res);
    bool ok = j && *jstr(j, "Id");
    if (ok && *jstr(j, "ServerName"))
        copy_str(server_name, sizeof(server_name), jstr(j, "ServerName"));
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
    char old_url[200];
    copy_str(old_url, sizeof(old_url), server_url);
    copy_str(server_url, sizeof(server_url), addr);
    server_name[0] = '\0';
    if (!check_server()) {
        copy_str(server_url, sizeof(server_url), old_url);
        show_message("Fant ingen Jellyfin-server på den adressen", 3500);
        return;
    }
    // the same server again, or a new one in the list
    int found = -1;
    for (int i = 0; i < account_count; i++)
        if (!strcmp(accounts[i].url, addr))
            found = i;
    char name[96];
    copy_str(name, sizeof(name), server_name);
    if (found >= 0)
        account_load(found);
    else if (account_count < MAX_ACCOUNTS) {
        account_sel = -1;
        copy_str(server_url, sizeof(server_url), addr);
        token[0] = user_id[0] = user_name[0] = '\0';
    }
    copy_str(server_name, sizeof(server_name), name);
    if (*token) {
        offline = false;
        load_views();
        return;
    }
    save_config();
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
    cJSON *pos = ud ? cJSON_GetObjectItem(ud, "PlaybackPositionTicks") : NULL;
    it->resume_s = cJSON_IsNumber(pos) ? (long)(pos->valuedouble / 10000000.0) : 0;
    cJSON *year = cJSON_GetObjectItem(o, "ProductionYear");
    cJSON *idx = cJSON_GetObjectItem(o, "IndexNumber");
    cJSON *pidx = cJSON_GetObjectItem(o, "ParentIndexNumber");
    it->order = (cJSON_IsNumber(pidx) ? pidx->valueint * 1000 : 0) + (cJSON_IsNumber(idx) ? idx->valueint : 0);
    if (*jstr(o, "SeriesName")) {
        copy_str(it->group, sizeof(it->group), jstr(o, "SeriesName"));
        copy_str(it->group_id, sizeof(it->group_id), jstr(o, "SeriesId"));
    }
    else if (*jstr(o, "Album")) {
        copy_str(it->group, sizeof(it->group), jstr(o, "Album"));
        copy_str(it->group_id, sizeof(it->group_id), jstr(o, "AlbumId"));
    }
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

static bool push_local(const char *group_id, const char *title);

// Load the children of parent_id into a new level on the stack
static bool push_level(const char *parent_id, const char *title, const char *collection)
{
    if (depth >= MAX_DEPTH)
        return false;
    if (!strncmp(parent_id, "local:", 6))
        return push_local(parent_id + 6, title);
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
    if (!strcmp(views[idx].collection, "downloads"))
        push_local("", views[idx].name);
    else
        push_level(views[idx].id, views[idx].name, views[idx].collection);
}

static int dl_count_done(void);

// Add the "Nedlastet" tab when something has been downloaded
static void add_downloads_view(void)
{
    if (view_count >= 8 || dl_count_done() == 0)
        return;
    View *v = &views[view_count++];
    copy_str(v->id, sizeof(v->id), "local:");
    copy_str(v->name, sizeof(v->name), "Nedlastet");
    copy_str(v->collection, sizeof(v->collection), "downloads");
}

// No server: show only what has been downloaded
static void go_offline(void)
{
    offline = true;
    view_count = 0;
    add_downloads_view();
    scr = S_HOME;
    open_view(0);
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
    offline = false;
    cJSON *arr = cJSON_GetObjectItem(j, "Items"), *o;
    cJSON_ArrayForEach(o, arr)
    {
        if (view_count >= 7)
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
    add_downloads_view();
    scr = S_HOME;
    open_view(0);
}

/* ---------- images ---------- */

static void img_path(Item *it, char *out, size_t n)
{
    if (*it->local || !strcmp(it->type, "LocalGroup")) {
        snprintf(out, n, "%s/%s.png", dl_dir, it->id);
        if (access(out, R_OK) != 0 && *it->group_id)
            snprintf(out, n, "%s/%s.png", dl_dir, it->group_id);
        return;
    }
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
    if (*it->local || !strcmp(it->type, "LocalGroup") || offline) {
        it->img_state = 3;
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

/* ---------- downloads ---------- */

// Downloads run in a small shell worker that outlives the app (it keeps
// going while a video plays). Each job is a file in SDCARD/Jellyfin/.queue;
// the worker downloads to <id>.part and renames it when done.

static const char *dl_worker_sh =
    "#!/bin/sh\n"
    "# Jellyfin download worker: args queue_dir dl_dir curl\n"
    "q=\"$1\"; dir=\"$2\"; curl=\"$3\"\n"
    "echo $$ > \"$dir/.worker.pid\"\n"
    "while :; do\n"
    "  job=$(ls \"$q\" 2> /dev/null | head -n 1)\n"
    "  [ -n \"$job\" ] || break\n"
    "  f=\"$q/$job\"\n"
    "  id=$(sed -n 1p \"$f\"); ext=$(sed -n 2p \"$f\"); auth=$(sed -n 3p \"$f\"); url=$(sed -n 4p \"$f\")\n"
    "  \"$curl\" -s -k -f -m 21600 -H \"Authorization: $auth\" -o \"$dir/$id.part\" \"$url\" &\n"
    "  c=$!\n"
    "  while kill -0 $c 2> /dev/null; do [ -f \"$f\" ] || kill $c; sleep 1; done\n"
    "  if wait $c && [ -f \"$f\" ]; then\n"
    "    mv \"$dir/$id.part\" \"$dir/$id.$ext\"\n"
    "  else\n"
    "    rm -f \"$dir/$id.part\"\n"
    "  fi\n"
    "  rm -f \"$f\"\n"
    "done\n"
    "rm -f \"$dir/.worker.pid\"\n";

typedef struct {
    char id[40];
    int state; // 1 queued, 2 downloading, 3 done
    long size, est;
} DlEntry;

static DlEntry dl_list[MAX_DL];
static int dl_n = 0;
static Uint32 dl_scanned = 0;
static long dl_bytes = 0;

static void dl_queue_dir(char *out, size_t n) { snprintf(out, n, "%s/.queue", dl_dir); }

static bool worker_running(void)
{
    char p[400], line[32];
    snprintf(p, sizeof(p), "%s/.worker.pid", dl_dir);
    read_file_line(p, line, sizeof(line));
    int pid = atoi(line);
    return pid > 0 && kill(pid, 0) == 0;
}

static void start_worker(void)
{
    if (worker_running())
        return;
    char script[400], q[400];
    snprintf(script, sizeof(script), "%s/dl_worker.sh", workdir);
    FILE *f = fopen(script, "w");
    if (!f)
        return;
    fputs(dl_worker_sh, f);
    fclose(f);
    dl_queue_dir(q, sizeof(q));
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        execl("/bin/sh", "sh", script, q, dl_dir, curl_bin, (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static DlEntry *dl_find(const char *id)
{
    for (int i = 0; i < dl_n; i++)
        if (!strcmp(dl_list[i].id, id))
            return &dl_list[i];
    return NULL;
}

static DlEntry *dl_add(const char *id)
{
    DlEntry *e = dl_find(id);
    if (e || dl_n >= MAX_DL)
        return e;
    e = &dl_list[dl_n++];
    memset(e, 0, sizeof(*e));
    copy_str(e->id, sizeof(e->id), id);
    return e;
}

// Read an item's saved metadata (id.json) into an Item
static bool dl_read_meta(const char *id, Item *it)
{
    char p[400];
    snprintf(p, sizeof(p), "%s/%s.json", dl_dir, id);
    char *data = read_all(p);
    if (!data)
        return false;
    cJSON *j = cJSON_Parse(data);
    free(data);
    if (!j)
        return false;
    memset(it, 0, sizeof(*it));
    copy_str(it->id, sizeof(it->id), id);
    copy_str(it->name, sizeof(it->name), jstr(j, "name"));
    copy_str(it->type, sizeof(it->type), jstr(j, "type"));
    copy_str(it->overview, sizeof(it->overview), jstr(j, "overview"));
    copy_str(it->sub, sizeof(it->sub), jstr(j, "sub"));
    copy_str(it->group, sizeof(it->group), jstr(j, "group"));
    copy_str(it->group_id, sizeof(it->group_id), jstr(j, "group_id"));
    cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "runtime")))
        it->runtime_ticks = (long)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "order")))
        it->order = v->valueint;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "est")))
        it->resume_s = (long)v->valuedouble; // borrowed: estimated size
    const char *ext = !strcmp(it->type, "Audio") ? "mp3" : "ts";
    snprintf(it->local, sizeof(it->local), "%s/%s.%s", dl_dir, id, ext);
    cJSON_Delete(j);
    return true;
}

// Scan SDCARD/Jellyfin (at most once a second) for queued, running and
// finished downloads
static void dl_scan(bool force)
{
    if (!force && SDL_GetTicks() - dl_scanned < 1000 && dl_scanned)
        return;
    dl_scanned = SDL_GetTicks();
    dl_n = 0;
    dl_bytes = 0;
    DIR *d = opendir(dl_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *dot = strrchr(e->d_name, '.');
            if (!dot || e->d_name[0] == '.')
                continue;
            bool media = !strcmp(dot, ".ts") || !strcmp(dot, ".mp3");
            bool part = !strcmp(dot, ".part");
            if (!media && !part)
                continue;
            char id[40];
            snprintf(id, sizeof(id), "%.*s", (int)(dot - e->d_name), e->d_name);
            char p[400];
            snprintf(p, sizeof(p), "%s/%s", dl_dir, e->d_name);
            struct stat st;
            long size = stat(p, &st) == 0 ? (long)st.st_size : 0;
            dl_bytes += size;
            DlEntry *x = dl_add(id);
            if (!x)
                continue;
            x->state = media ? 3 : 2;
            x->size = size;
            if (part) {
                Item meta;
                if (dl_read_meta(id, &meta))
                    x->est = meta.resume_s;
            }
        }
        closedir(d);
    }
    char q[400];
    dl_queue_dir(q, sizeof(q));
    d = opendir(q);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            // job files are named <seq>_<id>.job
            char *us = strchr(e->d_name, '_'), *dot = strrchr(e->d_name, '.');
            if (!us || !dot || strcmp(dot, ".job"))
                continue;
            char id[40];
            snprintf(id, sizeof(id), "%.*s", (int)(dot - us - 1), us + 1);
            DlEntry *x = dl_find(id);
            if (!x && (x = dl_add(id)))
                x->state = 1;
        }
        closedir(d);
    }
}

static int dl_count_done(void)
{
    dl_scan(false);
    int n = 0;
    for (int i = 0; i < dl_n; i++)
        n += dl_list[i].state == 3;
    return n;
}

static int dl_state(const char *id, int *pct)
{
    DlEntry *e = dl_find(id);
    if (!e)
        return 0;
    if (pct)
        *pct = e->est > 0 ? (int)(e->size * 100 / e->est) : 0;
    if (pct && *pct > 99)
        *pct = 99;
    return e->state;
}

// Short download status for a card or row: "Lagret", "42%" or "Kø"
static bool dl_badge(const char *id, char *out, size_t n)
{
    int pct = 0, st = dl_state(id, &pct);
    if (st == 3)
        snprintf(out, n, "Lagret");
    else if (st == 2)
        snprintf(out, n, "%d%%", pct);
    else if (st == 1)
        snprintf(out, n, "Kø");
    return st != 0;
}

static void dl_add_job(const char *id, const char *ext, const char *url)
{
    char q[400], p[500], val[360];
    dl_queue_dir(q, sizeof(q));
    mkdir_p(q);
    static int seq = 0;
    snprintf(p, sizeof(p), "%s/%010ld%03d_%s.job", q, (long)time(NULL), seq++ % 1000, id);
    auth_value(val, sizeof(val));
    FILE *f = fopen(p, "w");
    if (!f)
        return;
    fprintf(f, "%s\n%s\n%s\n%s\n", id, ext, val, url);
    fclose(f);
}

// Queue one playable item (film, episode or track). Returns 1 when queued,
// 2 when it is already downloaded or queued, 0 when it cannot be downloaded.
static int dl_queue_item(Item *it)
{
    bool audio = !strcmp(it->type, "Audio");
    if (strcmp(it->type, "Movie") && strcmp(it->type, "Episode") && strcmp(it->type, "Video") && !audio)
        return 0;
    if (dl_state(it->id, NULL))
        return 2; // already there or on its way
    mkdir_p(dl_dir);
    long est = audio ? 24000L * it->runtime_ticks
                     : (long)((qualities[quality].bitrate + 128000L) / 8) * it->runtime_ticks;
    // metadata for offline use
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", it->name);
    cJSON_AddStringToObject(j, "type", it->type);
    cJSON_AddStringToObject(j, "overview", it->overview);
    cJSON_AddStringToObject(j, "sub", it->sub);
    cJSON_AddStringToObject(j, "group", it->group);
    cJSON_AddStringToObject(j, "group_id", it->group_id);
    cJSON_AddNumberToObject(j, "runtime", it->runtime_ticks);
    cJSON_AddNumberToObject(j, "order", it->order);
    cJSON_AddNumberToObject(j, "est", est);
    char *txt = cJSON_Print(j);
    cJSON_Delete(j);
    char p[400];
    snprintf(p, sizeof(p), "%s/%s.json", dl_dir, it->id);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
    free(txt);

    char url[900];
    if (audio)
        snprintf(url, sizeof(url),
                 "%s/Audio/%s/stream.mp3?api_key=%s&AudioCodec=mp3&AudioBitrate=192000&Static=false&DeviceId=%s"
                 "&PlaySessionId=dl%s",
                 server_url, it->id, token, device_id, it->id);
    else
        snprintf(url, sizeof(url),
                 "%s/Videos/%s/stream.ts?api_key=%s&VideoCodec=h264&AudioCodec=mp3&MaxWidth=%d&MaxHeight=%d"
                 "&VideoBitrate=%d&AudioBitrate=128000&AudioChannels=2&TranscodingMaxAudioChannels=2"
                 "&h264-profile=baseline&h264-level=30&Static=false&MediaSourceId=%s&DeviceId=%s&PlaySessionId=dl%s",
                 server_url, it->id, token, qualities[quality].max_w, qualities[quality].max_h,
                 qualities[quality].bitrate, it->id, device_id, it->id);
    // posters: the item's own, and the series/album one for the folder view
    char img[600];
    const char *img_id = audio && *it->group_id ? it->group_id : it->id;
    snprintf(p, sizeof(p), "%s/%s.png", dl_dir, img_id);
    if (access(p, R_OK) != 0) {
        snprintf(img, sizeof(img), "%s/Items/%s/Images/Primary?fillWidth=180&fillHeight=253&format=Png&api_key=%s",
                 server_url, img_id, token);
        char src[400];
        snprintf(src, sizeof(src), "%s/img/%s.png", workdir, img_id);
        char *cached = read_all(src);
        if (cached) { // already have it from browsing
            free(cached);
            char cmd[900];
            snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, p);
            if (system(cmd) != 0)
                dl_add_job(img_id, "png", img);
        }
        else
            dl_add_job(img_id, "png", img);
    }
    if (*it->group_id && !audio) {
        snprintf(p, sizeof(p), "%s/%s.png", dl_dir, it->group_id);
        if (access(p, R_OK) != 0) {
            snprintf(img, sizeof(img),
                     "%s/Items/%s/Images/Primary?fillWidth=180&fillHeight=253&format=Png&api_key=%s", server_url,
                     it->group_id, token);
            dl_add_job(it->group_id, "png", img);
        }
    }
    dl_add_job(it->id, audio ? "mp3" : "ts", url);
    return 1;
}

// Queue an item, or everything in a series, season or album
static void dl_queue_selection(Item *it)
{
    int n = 0, have = 0, r;
    if (!it->is_folder) {
        r = dl_queue_item(it);
        n += r == 1;
        have += r == 2;
    }
    else {
        char path[400];
        snprintf(path, sizeof(path),
                 "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=Episode,Audio,Movie"
                 "&SortBy=ParentIndexNumber,IndexNumber,SortName&Fields=Overview&Limit=%d",
                 user_id, it->id, MAX_ITEMS);
        cJSON *j = api_json("GET", path, NULL), *o;
        Item tmp;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(j, "Items"))
        {
            fill_item(&tmp, o);
            if (!*tmp.group && strcmp(tmp.type, "Movie")) {
                copy_str(tmp.group, sizeof(tmp.group), it->name);
                copy_str(tmp.group_id, sizeof(tmp.group_id), it->id);
            }
            r = dl_queue_item(&tmp);
            n += r == 1;
            have += r == 2;
        }
        cJSON_Delete(j);
    }
    if (n > 0) {
        start_worker();
        dl_scan(true);
        char m[80];
        if (n == 1)
            snprintf(m, sizeof(m), "Lastes ned: %s", it->name);
        else
            snprintf(m, sizeof(m), "%d filer lagt i nedlastingskøen", n);
        show_message(m, 2500);
    }
    else if (have)
        show_message("Allerede lastet ned", 2500);
    else
        show_message("Dette kan ikke lastes ned", 2500);
}

static void dl_remove_files(const char *id)
{
    static const char *exts[] = {"ts", "mp3", "part", "json", "png"};
    char p[400];
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof(p), "%s/%s.%s", dl_dir, id, exts[i]);
        remove(p);
    }
    char q[400];
    dl_queue_dir(q, sizeof(q));
    DIR *d = opendir(q);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *us = strchr(e->d_name, '_');
            if (us && !strncmp(us + 1, id, strlen(id)) && us[1 + strlen(id)] == '.') {
                snprintf(p, sizeof(p), "%s/%s", q, e->d_name);
                remove(p);
            }
        }
        closedir(d);
    }
}

// Delete a downloaded item, or every item in a series/album folder
static void dl_delete(Item *it)
{
    if (!strcmp(it->type, "LocalGroup")) {
        DIR *d = opendir(dl_dir);
        if (d) {
            struct dirent *e;
            char ids[MAX_DL][40];
            int n = 0;
            while ((e = readdir(d)) && n < MAX_DL) {
                char *dot = strrchr(e->d_name, '.');
                if (!dot || strcmp(dot, ".json"))
                    continue;
                snprintf(ids[n], sizeof(ids[n]), "%.*s", (int)(dot - e->d_name), e->d_name);
                Item meta;
                if (dl_read_meta(ids[n], &meta) && !strcmp(meta.group_id, it->id))
                    n++;
            }
            closedir(d);
            for (int i = 0; i < n; i++)
                dl_remove_files(ids[i]);
        }
    }
    dl_remove_files(it->id);
    dl_scan(true);
}

static void dl_delete_all(void)
{
    // the worker stops its download when the job file is gone
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dl_dir);
    if (system(cmd) != 0)
        show_message("Kunne ikke slette alt", 2500);
    dl_scan(true);
}

static int cmp_items(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->order != y->order && *x->group && !strcmp(x->group_id, y->group_id))
        return x->order - y->order;
    return strcasecmp(x->name, y->name);
}

// A level with downloaded items: films and series/album folders at the top
// (group_id ""), or the items of one folder
static bool push_local(const char *group_id, const char *title)
{
    if (depth >= MAX_DEPTH)
        return false;
    dl_scan(true);
    Level *lv = &stack[depth];
    memset(lv, 0, sizeof(*lv));
    snprintf(lv->parent_id, sizeof(lv->parent_id), "local:%s", group_id);
    copy_str(lv->title, sizeof(lv->title), title);
    lv->items = calloc(MAX_ITEMS, sizeof(Item));
    for (int i = 0; i < dl_n && lv->count < MAX_ITEMS; i++) {
        if (dl_list[i].state != 3)
            continue;
        Item meta;
        if (!dl_read_meta(dl_list[i].id, &meta))
            continue;
        meta.resume_s = 0;
        if (*group_id) {
            if (strcmp(meta.group_id, group_id))
                continue;
            lv->items[lv->count++] = meta;
        }
        else if (*meta.group_id) {
            bool have = false;
            for (int k = 0; k < lv->count; k++)
                if (!strcmp(lv->items[k].id, meta.group_id))
                    have = true;
            if (have)
                continue;
            Item *g = &lv->items[lv->count++];
            memset(g, 0, sizeof(*g));
            copy_str(g->id, sizeof(g->id), meta.group_id);
            copy_str(g->name, sizeof(g->name), meta.group);
            copy_str(g->type, sizeof(g->type), "LocalGroup");
            g->is_folder = true;
        }
        else
            lv->items[lv->count++] = meta;
    }
    qsort(lv->items, lv->count, sizeof(Item), cmp_items);
    lv->list_mode = *group_id && lv->count > 0;
    depth++;
    return true;
}

static bool level_is_local(Level *lv) { return !strncmp(lv->parent_id, "local:", 6); }

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
    Item meta;
    bool local = *it->local || (dl_state(it->id, NULL) == 3 && dl_read_meta(it->id, &meta));
    if (local)
        copy_str(url, sizeof(url), *it->local ? it->local : meta.local);
    else
        snprintf(url, sizeof(url),
                 "%s/Audio/%s/stream.mp3?api_key=%s&ApiKey=%s&AudioCodec=mp3&AudioBitrate=192000&Static=false&DeviceId=%s",
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
        else if (local)
            execl("./bin/ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-i", url, (char *)NULL);
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

// Write the play file for jfplayer (see src/jfplayer) and leave the app;
// launch.sh starts the player and then this app again.
static void play_video(Item *it, long start_s)
{
    music_stop();
    // a downloaded copy plays from the memory card, also when online
    Item local_copy;
    if (!*it->local && dl_state(it->id, NULL) == 3 && dl_read_meta(it->id, &local_copy)) {
        if (access(local_copy.local, R_OK) == 0) {
            FILE *fp = fopen("/tmp/jellyfin_play", "w");
            if (fp) {
                fprintf(fp, "url=%s\nlocal=1\ntitle=%s\nduration=%ld\nstart=%ld\n", local_copy.local, it->name,
                        it->runtime_ticks, start_s);
                char val[360];
                auth_value(val, sizeof(val));
                if (*token) // keep reporting progress to the server
                    fprintf(fp, "auth=%s\nserver=%s\nitem=%s\n", val, server_url, it->id);
                fclose(fp);
                goto state;
            }
        }
    }
    if (*it->local) {
        FILE *fp = fopen("/tmp/jellyfin_play", "w");
        if (!fp) {
            show_message("Kunne ikke starte avspilling", 3000);
            return;
        }
        fprintf(fp, "url=%s\nlocal=1\ntitle=%s\nduration=%ld\nstart=%ld\n", it->local, it->name, it->runtime_ticks,
                start_s);
        if (!offline && *token) {
            char val[360];
            auth_value(val, sizeof(val));
            fprintf(fp, "auth=%s\nserver=%s\nitem=%s\n", val, server_url, it->id);
        }
        fclose(fp);
        goto state;
    }
    FILE *fp = fopen("/tmp/jellyfin_play", "w");
    if (!fp) {
        show_message("Kunne ikke starte avspilling", 3000);
        return;
    }
    char val[360];
    auth_value(val, sizeof(val));
    fprintf(fp,
            "url=%s/Videos/%s/stream.ts?api_key=%s&ApiKey=%s&VideoCodec=h264&AudioCodec=mp3&MaxWidth=%d"
            "&MaxHeight=%d&VideoBitrate=%d&AudioBitrate=128000&AudioChannels=2&TranscodingMaxAudioChannels=2"
            "&h264-profile=baseline&h264-level=30&Static=false&MediaSourceId=%s&DeviceId=%s\n",
            server_url, it->id, token, token, qualities[quality].max_w, qualities[quality].max_h,
            qualities[quality].bitrate, it->id, device_id);
    fprintf(fp, "auth=%s\nserver=%s\nitem=%s\ntitle=%s\nduration=%ld\nstart=%ld\n", val, server_url, it->id,
            it->name, it->runtime_ticks, start_s);
    // audio and subtitle tracks, for the menus in the player
    char path[200];
    snprintf(path, sizeof(path), "/Users/%s/Items/%s", user_id, it->id);
    cJSON *d = api_json("GET", path, NULL);
    cJSON *srcs = d ? cJSON_GetObjectItem(d, "MediaSources") : NULL;
    cJSON *streams = cJSON_GetArraySize(srcs) ? cJSON_GetObjectItem(cJSON_GetArrayItem(srcs, 0), "MediaStreams") : NULL;
    int audio_sel = -1;
    cJSON *st;
    cJSON_ArrayForEach(st, streams)
    {
        const char *type = jstr(st, "Type");
        cJSON *idx = cJSON_GetObjectItem(st, "Index");
        if (!cJSON_IsNumber(idx))
            continue;
        char name[120];
        copy_str(name, sizeof(name), jstr(st, "DisplayTitle"));
        for (char *c = name; *c; c++)
            if (*c == '\n' || *c == '\r')
                *c = ' ';
        if (!strcmp(type, "Audio")) {
            fprintf(fp, "audio=%d|%s\n", idx->valueint, name);
            if (audio_sel < 0 || cJSON_IsTrue(cJSON_GetObjectItem(st, "IsDefault")))
                audio_sel = idx->valueint;
        }
        else if (!strcmp(type, "Subtitle"))
            fprintf(fp, "sub=%d|%s\n", idx->valueint, name);
    }
    cJSON_Delete(d);
    fprintf(fp, "audio_sel=%d\nsub_sel=-1\n", audio_sel);
    fclose(fp);
state:
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
        else if (!offline || !strncmp(line, "local:", 6))
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

static bool can_download(Item *it)
{
    if (offline || *it->local)
        return false;
    if (it->is_folder)
        return strcmp(it->type, "CollectionFolder") && strcmp(it->type, "Folder") && strcmp(it->type, "LocalGroup");
    return !strcmp(it->type, "Movie") || !strcmp(it->type, "Episode") || !strcmp(it->type, "Video") ||
           !strcmp(it->type, "Audio");
}

static void draw_hint_bar(void)
{
    const char *k[6], *l[6];
    int n = 0;
#define HINT(a, b) (k[n] = (a), l[n] = (b), n++)
    if (scr == S_DISCOVER) {
        HINT("A", "Velg");
        HINT("Y", "Søk igjen");
        HINT("B", account_count > 0 ? "Tilbake" : "Avslutt");
    }
    else if (scr == S_LOGIN) {
        HINT("Y", "Passord");
        HINT("X", "Ny kode");
        HINT("B", "Tilbake");
        HINT("SELECT", "Innstillinger");
    }
    else if (scr == S_SETTINGS) {
        if (settings_sel == SET_QUALITY)
            HINT("←→", "Endre");
        else if (settings_sel != SET_ABOUT && settings_sel != SET_USER)
            HINT("A", "Velg");
        else if (settings_sel == SET_USER && !offline)
            HINT("A", *token ? "Logg ut" : "Logg inn");
        HINT("B", "Tilbake");
    }
    else if (scr == S_SERVERS) {
        HINT("A", servers_sel < account_count ? "Bruk" : "Legg til");
        if (servers_sel < account_count)
            HINT("Y", "Fjern");
        HINT("B", "Tilbake");
    }
    else if (scr == S_DETAIL) {
        bool resume = detail_item && detail_item->resume_s > 30 && !*detail_item->local;
        HINT("A", resume ? "Fortsett" : "Spill av");
        if (resume)
            HINT("Y", "Fra start");
        if (*detail_item->local)
            HINT("Y", "Slett");
        else if (can_download(detail_item) && !dl_state(detail_item->id, NULL))
            HINT("X", "Last ned");
        HINT("B", "Tilbake");
        HINT("SELECT", "Meny");
    }
    else {
        Level *lv = depth ? &stack[depth - 1] : NULL;
        Item *it = lv && lv->count ? &lv->items[lv->sel] : NULL;
        HINT("A", lv && lv->list_mode && lv->count && !strcmp(lv->items[0].type, "Audio") ? "Spill" : "Åpne");
        HINT("B", depth > 1 ? "Tilbake" : "Avslutt");
        if (depth == 1 && view_count > 1)
            HINT("L/R", "Bibliotek");
        if (music_pid > 0)
            HINT("Y", "Stopp musikk");
        else if (it && lv && level_is_local(lv))
            HINT("Y", "Slett");
        else if (it && can_download(it))
            HINT("X", "Last ned");
        HINT("SELECT", "Meny");
    }
    draw_hints(k, l, n);
#undef HINT
}

// "onionsoup" and the library tabs. The tabs scroll so the selected one is
// always visible; the server name is not shown (it collided with the tabs).
static void draw_topbar_tabs(void)
{
    soup_logo("");
    int x0 = 18 + text_w(f24, "onionsoup") + 30, x1 = SCREEN_W - 12;
    if (offline) {
        int w = text_w(f13, "OFFLINE") + 16;
        x1 -= w;
        fill_round(x1 + 4, 16, w - 4, 20, 10, C_PANEL);
        text(f13, "OFFLINE", x1 + 12, 18, C_GREY);
        x1 -= 8;
    }
    if (scr != S_HOME && scr != S_DETAIL)
        return;
    int pos[8], wid[8], total = 0;
    for (int i = 0; i < view_count; i++) {
        wid[i] = text_w(f17, views[i].name);
        pos[i] = total;
        total += wid[i] + 26;
    }
    total -= 26;
    int off = 0;
    if (total > x1 - x0 && view_count > 0) {
        // keep the selected tab inside, with its neighbour peeking in
        off = pos[view_sel] + wid[view_sel] / 2 - (x1 - x0) / 2;
        if (off > total - (x1 - x0))
            off = total - (x1 - x0);
        if (off < 0)
            off = 0;
    }
    else if (view_count > 0)
        off = total - (x1 - x0); // right aligned when everything fits
    SDL_Rect clip = {x0 - 10, 0, x1 - x0 + 20, TOP_H};
    SDL_SetClipRect(screen, &clip);
    for (int i = 0; i < view_count; i++) {
        int tx = x0 + pos[i] - off;
        if (i == view_sel) {
            bool hl = depth == 1 && scr == S_HOME;
            fill_round(tx - 10, 12, wid[i] + 20, 28, 14, hl ? C_ACCENT : C_PANEL);
            text(f17, views[i].name, tx, 15, hl ? C_DARK : C_ACCENT);
        }
        else
            text(f17, views[i].name, tx, 15, C_GREY);
    }
    SDL_SetClipRect(screen, NULL);
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
    char badge[16];
    if (!*it->local && dl_badge(it->id, badge, sizeof(badge))) {
        int bw = text_w(f13, badge) + 12;
        fill_round(x + 5, y + 5, bw, 18, 9, C_DARK);
        text(f13, badge, x + 11, y + 6, C_ACCENT);
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
        text_fit(f17, it->name, 70, y + 8, SCREEN_W - 270, playing ? C_ACCENT : (s ? C_WHITE : C_SOFT));
        char dur[32];
        format_duration(it->runtime_ticks, dur, sizeof(dur));
        int dx = SCREEN_W - 40;
        if (it->runtime_ticks) {
            dx -= text_w(f15, dur);
            text(f15, dur, dx, y + 10, C_GREY);
        }
        char badge[16];
        if (!*it->local && dl_badge(it->id, badge, sizeof(badge)))
            text(f13, badge, dx - 16 - text_w(f13, badge), y + 12, C_ACCENT);
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
    char label[48] = "Spill av";
    if (it->resume_s > 30) {
        long r = it->resume_s;
        if (r >= 3600)
            snprintf(label, sizeof(label), "Fortsett fra %ld:%02ld:%02ld", r / 3600, r / 60 % 60, r % 60);
        else
            snprintf(label, sizeof(label), "Fortsett fra %ld:%02ld", r / 60, r % 60);
    }
    int lw = text_w(f19, label) + 40;
    fill_round(x, by, lw, 36, 18, C_ACCENT);
    text(f19, label, x + 20, by + 6, C_DARK);
    if (it->resume_s > 30 && it->runtime_ticks > 0) {
        int pw = lw - 36;
        fill_round(x + 18, by + 42, pw, 4, 2, C_CARD);
        int fw = (int)(pw * (double)it->resume_s / it->runtime_ticks);
        if (fw > 0)
            fill_round(x + 18, by + 42, fw > pw ? pw : fw, 4, 2, C_ACCENT);
    }
    else if (it->played)
        text(f15, "Sett", x + lw + 16, by + 9, C_GREEN);
    char status[160];
    int pct = 0, st = *it->local ? 3 : dl_state(it->id, &pct);
    if (st == 3)
        snprintf(status, sizeof(status), "Lagret på minnekortet%s", offline ? "" : ". Spilles av fra kortet.");
    else if (st == 2)
        snprintf(status, sizeof(status), "Lastes ned … %d%%", pct);
    else if (st == 1)
        snprintf(status, sizeof(status), "Venter i nedlastingskøen");
    else
        snprintf(status, sizeof(status), "Video omkodes av serveren til %s for Miyoo.", qualities[quality].name);
    text_fit(f13, status, 24, TOP_H + 300, SCREEN_W - 48, st ? C_ACCENT : C_GREY);
}

/* ---------- settings ---------- */

static bool confirm_pending(const char *what)
{
    return SDL_GetTicks() < confirm_until && !strcmp(confirm_what, what);
}

// A destructive action needs a second press within 3 seconds
static bool confirm(const char *what, const char *question)
{
    if (confirm_pending(what)) {
        confirm_until = 0;
        return true;
    }
    copy_str(confirm_what, sizeof(confirm_what), what);
    confirm_until = SDL_GetTicks() + 3000;
    if (scr != S_SETTINGS && scr != S_SERVERS) // those screens ask in the row itself
        show_message(question, 3000);
    return false;
}

static void open_settings(void)
{
    if (scr != S_SETTINGS && scr != S_SERVERS)
        prev_scr = scr == S_DETAIL || scr == S_HOME ? scr : S_HOME;
    settings_sel = 0;
    dl_scan(true);
    scr = S_SETTINGS;
}

static void close_settings(void)
{
    if (depth > 0)
        scr = prev_scr == S_DETAIL && detail_item ? S_DETAIL : S_HOME;
    else if (*server_url && !*token && !offline)
        scr = S_LOGIN;
    else if (dl_count_done() > 0) {
        go_offline();
    }
    else {
        scr = S_DISCOVER;
        discovery_start();
    }
}

static void settings_value(int row, char *out, size_t n)
{
    out[0] = '\0';
    switch (row) {
    case SET_SERVER:
        if (offline)
            snprintf(out, n, "Ikke tilkoblet");
        else
            snprintf(out, n, "%s", *server_name ? server_name : server_url);
        break;
    case SET_USER:
        snprintf(out, n, "%s", offline ? "–" : *token ? (*user_name ? user_name : "Innlogget")
                                                      : "Ikke innlogget");
        break;
    case SET_QUALITY:
        snprintf(out, n, "‹ %s ›", qualities[quality].name);
        break;
    case SET_DOWNLOADS: {
        int done = 0, waiting = 0;
        for (int i = 0; i < dl_n; i++)
            *(dl_list[i].state == 3 ? &done : &waiting) += 1;
        if (waiting)
            snprintf(out, n, "%d filer · %.0f MB · %d i kø", done, dl_bytes / 1048576.0, waiting);
        else
            snprintf(out, n, "%d filer · %.0f MB", done, dl_bytes / 1048576.0);
        break;
    }
    case SET_ABOUT:
        snprintf(out, n, "Jellyfin for OnionSoup " APP_VERSION);
        break;
    }
}

static void draw_settings(void)
{
    static const char *labels[SET_COUNT] = {"Server", "Bruker", "Videokvalitet", "Nedlastinger", "Slett alle nedlastinger", "Om"};
    text(f24, "Innstillinger", 24, TOP_H + 12, C_WHITE);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = TOP_H + 52 + i * 54;
        bool s = i == settings_sel;
        fill_round(24, y, SCREEN_W - 48, 46, 12, s ? C_CARD : C_PANEL);
        if (s)
            fill_round(24, y, 6, 46, 3, C_ACCENT);
        bool danger = i == SET_DELETE_ALL;
        const char *label = labels[i];
        if (danger && confirm_pending("all"))
            label = "Trykk A igjen for å slette alt";
        text(f19, label, 44, y + 11, danger ? C_RED : (s ? C_WHITE : C_SOFT));
        char val[160];
        settings_value(i, val, sizeof(val));
        int vw = text_w(f15, val);
        int maxw = SCREEN_W - 80 - text_w(f19, label) - 40;
        if (vw > maxw)
            vw = maxw;
        text_fit(f15, val, SCREEN_W - 44 - vw, y + 14, maxw, i == SET_QUALITY && s ? C_ACCENT : C_GREY);
    }
}

static void draw_servers(void)
{
    text(f24, "Servere", 24, TOP_H + 12, C_WHITE);
    int rows = account_count + 1;
    int visible = 6;
    int first = servers_sel >= visible ? servers_sel - visible + 1 : 0;
    for (int k = 0; k < visible && first + k < rows; k++) {
        int i = first + k;
        int y = TOP_H + 52 + k * 58;
        bool s = i == servers_sel;
        fill_round(24, y, SCREEN_W - 48, 50, 12, s ? C_CARD : C_PANEL);
        if (s)
            fill_round(24, y, 6, 50, 3, C_ACCENT);
        if (i < account_count) {
            Account *a = &accounts[i];
            bool cur = i == account_sel && !offline;
            char line[200];
            snprintf(line, sizeof(line), "%s", *a->name ? a->name : "Jellyfin");
            text_fit(f19, line, 44, y + 5, SCREEN_W - 200, C_WHITE);
            snprintf(line, sizeof(line), "%s%s%s", *a->user_name ? a->user_name : "ikke innlogget", "  ·  ", a->url);
            text_fit(f13, line, 44, y + 29, SCREEN_W - 200, C_GREY);
            if (cur)
                text(f15, "I bruk", SCREEN_W - 44 - text_w(f15, "I bruk"), y + 15, C_GREEN);
            if (s && confirm_pending("server"))
                text(f15, "Y igjen: fjern", SCREEN_W - 44 - text_w(f15, "Y igjen: fjern"), y + 15, C_RED);
        }
        else
            text(f19, "Legg til server …", 44, y + 13, s ? C_ACCENT : C_SOFT);
    }
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
    case S_SETTINGS:
        draw_settings();
        break;
    case S_SERVERS:
        draw_servers();
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
    else if (!strcmp(it->type, "LocalGroup")) {
        push_local(it->id, it->name);
    }
    else if (it->is_folder) {
        push_level(it->id, it->name, NULL);
    }
    else {
        detail_item = it;
        scr = S_DETAIL;
    }
}

// Reload the current downloads level after a delete, keeping the position
static void refresh_local_level(void)
{
    Level *lv = &stack[depth - 1];
    char gid[40], title[160];
    copy_str(gid, sizeof(gid), lv->parent_id + 6);
    copy_str(title, sizeof(title), lv->title);
    int sel = lv->sel;
    depth--;
    if (music_level == lv)
        music_level = NULL;
    free_level(lv);
    push_local(gid, title);
    lv = &stack[depth - 1];
    lv->sel = sel < lv->count ? sel : (lv->count ? lv->count - 1 : 0);
    // a folder that became empty: go up a level
    if (lv->count == 0 && depth > 1) {
        pop_level();
        refresh_local_level();
    }
    if (depth == 1 && lv->count == 0) {
        if (offline) {
            go_offline(); // nothing left: shows an empty downloads page
        }
        else
            load_views();
    }
}

// Switch to a saved server account
static void use_account(int i)
{
    music_stop();
    account_load(i);
    save_config();
    if (!check_server()) {
        show_message("Får ikke kontakt med serveren", 3000);
        return;
    }
    if (*token)
        load_views();
    else {
        scr = S_LOGIN;
        quick_connect_start();
    }
}

static void remove_account(int i)
{
    bool current = i == account_sel;
    for (int k = i; k < account_count - 1; k++)
        accounts[k] = accounts[k + 1];
    account_count--;
    if (current) {
        account_sel = -1;
        server_url[0] = server_name[0] = token[0] = user_id[0] = user_name[0] = '\0';
        view_count = 0;
        open_view(-1);
    }
    else if (account_sel > i)
        account_sel--;
    save_config();
    if (servers_sel > account_count)
        servers_sel = account_count;
}

static bool disc_from_settings = false;

static void on_settings_key(SDLKey key)
{
    if (key == SW_BTN_B || key == SW_BTN_MENU || key == SW_BTN_SELECT) {
        close_settings();
        return;
    }
    if (key == SW_BTN_UP && settings_sel > 0)
        settings_sel--;
    else if (key == SW_BTN_DOWN && settings_sel < SET_COUNT - 1)
        settings_sel++;
    else if (settings_sel == SET_QUALITY && (key == SW_BTN_LEFT || key == SW_BTN_RIGHT || key == SW_BTN_A)) {
        quality = (quality + (key == SW_BTN_LEFT ? 2 : 1)) % 3;
        save_config();
    }
    else if (key != SW_BTN_A)
        return;
    else if (settings_sel == SET_SERVER) {
        servers_sel = account_sel >= 0 ? account_sel : 0;
        scr = S_SERVERS;
    }
    else if (settings_sel == SET_USER && !offline && *server_url) {
        music_stop();
        if (*token) {
            sign_out();
            view_count = 0;
            open_view(-1);
            show_message("Logget ut", 2000);
        }
        scr = S_LOGIN;
        quick_connect_start();
    }
    else if (settings_sel == SET_DOWNLOADS) {
        if (dl_count_done() == 0) {
            show_message(dl_n ? "Ingenting er ferdig nedlastet ennå" : "Ingenting er lastet ned", 2500);
            return;
        }
        int idx = -1;
        for (int i = 0; i < view_count; i++)
            if (!strcmp(views[i].collection, "downloads"))
                idx = i;
        if (idx < 0) {
            add_downloads_view();
            idx = view_count - 1;
        }
        open_view(idx);
        scr = S_HOME;
    }
    else if (settings_sel == SET_DELETE_ALL) {
        if (!dl_n) {
            show_message("Ingenting å slette", 2000);
            return;
        }
        if (!confirm("all", "Trykk A igjen for å slette alle nedlastinger"))
            return;
        music_stop();
        bool on_local = depth > 0 && level_is_local(&stack[0]);
        dl_delete_all();
        // drop the downloads tab
        for (int i = 0; i < view_count; i++)
            if (!strcmp(views[i].collection, "downloads")) {
                view_count--;
                if (view_sel >= view_count)
                    view_sel = 0;
            }
        if (on_local || offline) {
            if (offline) {
                view_count = 0;
                open_view(-1);
            }
            else
                open_view(0);
        }
        show_message("Alle nedlastinger er slettet", 2500);
    }
}

static void on_servers_key(SDLKey key)
{
    if (key == SW_BTN_B || key == SW_BTN_MENU) {
        confirm_until = 0;
        scr = S_SETTINGS;
    }
    else if (key == SW_BTN_UP && servers_sel > 0)
        servers_sel--;
    else if (key == SW_BTN_DOWN && servers_sel < account_count)
        servers_sel++;
    else if (key == SW_BTN_A) {
        if (servers_sel < account_count)
            use_account(servers_sel);
        else {
            disc_from_settings = true;
            scr = S_DISCOVER;
            discovery_start();
        }
    }
    else if (key == SW_BTN_Y && servers_sel < account_count) {
        if (confirm("server", "Trykk Y igjen for å fjerne serveren"))
            remove_account(servers_sel);
    }
}

static void delete_selected(Item *it)
{
    char what[64];
    snprintf(what, sizeof(what), "del:%.50s", it->id);
    char q[200];
    snprintf(q, sizeof(q), "Trykk Y igjen for å slette «%.120s»", it->name);
    if (!confirm(what, q))
        return;
    if (music_pid > 0 && music_level && music_level == &stack[depth - 1])
        music_stop();
    dl_delete(it);
    show_message("Slettet", 1500);
}

static void on_key(SDLKey key)
{
    if (key == SW_BTN_START && music_pid > 0) {
        kill(music_pid, music_paused ? SIGCONT : SIGSTOP);
        music_paused = !music_paused;
        return;
    }
    if (key == SW_BTN_SELECT && (scr == S_HOME || scr == S_DETAIL || scr == S_LOGIN)) {
        open_settings();
        return;
    }
    switch (scr) {
    case S_SETTINGS:
        on_settings_key(key);
        break;
    case S_SERVERS:
        on_servers_key(key);
        break;
    case S_DISCOVER:
        if (key == SW_BTN_UP && server_sel > 0)
            server_sel--;
        else if (key == SW_BTN_DOWN && server_sel < server_count)
            server_sel++;
        else if (key == SW_BTN_Y)
            discovery_start();
        else if (key == SW_BTN_B || key == SW_BTN_MENU) {
            if (disc_from_settings || account_count > 0) {
                disc_from_settings = false;
                scr = S_SERVERS;
            }
            else
                quit = true;
        }
        else if (key == SW_BTN_A) {
            disc_from_settings = false;
            if (server_sel < server_count)
                choose_server(servers[server_sel].address);
            else {
                const char *addr = launch_keyboard("http://", "Serveradresse");
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
    case S_DETAIL: {
        Item *it = detail_item;
        bool resume = it->resume_s > 30 && !*it->local;
        if (key == SW_BTN_B)
            scr = S_HOME;
        else if (key == SW_BTN_A)
            play_video(it, resume ? it->resume_s : 0);
        else if (key == SW_BTN_Y && resume)
            play_video(it, 0);
        else if (key == SW_BTN_X && can_download(it))
            dl_queue_selection(it);
        else if (key == SW_BTN_Y && *it->local) {
            char what[64];
            snprintf(what, sizeof(what), "del:%.50s", it->id);
            bool go = confirm_pending(what);
            delete_selected(it);
            if (go) {
                scr = S_HOME;
                detail_item = NULL;
                refresh_local_level();
            }
        }
        break;
    }
    case S_HOME: {
        if (key == SW_BTN_Y && music_pid > 0) {
            music_stop();
            music_index = -1;
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
        else if (key == SW_BTN_X && can_download(&lv->items[lv->sel]))
            dl_queue_selection(&lv->items[lv->sel]);
        else if (key == SW_BTN_Y && level_is_local(lv)) {
            char what[64];
            snprintf(what, sizeof(what), "del:%.50s", lv->items[lv->sel].id);
            bool go = confirm_pending(what);
            delete_selected(&lv->items[lv->sel]);
            if (go)
                refresh_local_level();
        }
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
    if ((e = getenv("JF_DOWNLOADS")))
        copy_str(dl_dir, sizeof(dl_dir), e);
    else
        snprintf(dl_dir, sizeof(dl_dir), "%s/Jellyfin", sdcard);
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
    soup_logo("");
    text(f24, "Kobler til …", (SCREEN_W - text_w(f24, "Kobler til …")) / 2, 210, C_WHITE);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);

    if (*server_url && check_server()) {
        if (*token) {
            load_views();
            if (scr == S_HOME)
                restore_state();
        }
        else {
            scr = S_LOGIN;
            quick_connect_start();
        }
    }
    else if (dl_count_done() > 0) {
        // no server (no WiFi, or away from home): only what is downloaded
        go_offline();
        restore_state();
    }
    else {
        scr = S_DISCOVER;
        discovery_start();
    }

    // Redraw at once after input, otherwise ~8 times a second: a full redraw
    // every frame keeps the CPU busy and slows down background downloads
    Uint32 last_draw = 0;
    while (!quit) {
        bool dirty = spos != NULL;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                quit = true;
            else if (ev.type == SDL_KEYDOWN && !spos) {
                on_key(ev.key.keysym.sym);
                dirty = true;
            }
        }
        script_step();
        discovery_poll();
        if (scr == S_LOGIN)
            quick_connect_poll();
        poll_images();
        poll_music();
        dl_scan(false);
        // a download finished: show the "Nedlastet" tab
        if (!offline && scr == S_HOME && view_count > 0 && strcmp(views[view_count - 1].collection, "downloads"))
            add_downloads_view();
        Uint32 now = SDL_GetTicks();
        if (dirty || now - last_draw >= 120) {
            draw();
            last_draw = now;
        }
        SDL_Delay(20);
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

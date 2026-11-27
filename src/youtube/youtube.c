// OnionSoup YouTube app for Miyoo Mini / Mini+
//
// Talks to YouTube through Invidious (an open front end with a JSON API), so
// no Google account or API key is needed and the video comes as a plain MP4
// (itag 18: 360p H.264 + AAC) that the Miyoo can decode.
//
// - Tabs: Populært, Søk, Lagret (downloaded), Historikk
// - A plays: the app exits with code 10 and writes /tmp/youtube_play;
//   launch.sh downloads the stream with curl and plays it with jfplayer
//   while it downloads (seeking works in the part already downloaded)
// - X downloads to SDCARD/YouTube for offline viewing, Y removes
// - SELECT switches to the next Invidious server
//
// Servers are read from instances.txt next to the app (one https://... per
// line); if none answer, the public list from api.invidious.io is tried.
//
// Environment overrides for testing on a PC:
//   SDCARD, YT_DIR (app folder), YT_FONT, YT_CURL, YT_FFMPEG,
//   YT_KEYBOARD_INPUT (answers for the keyboard, split by '|'),
//   YT_SCRIPT (scripted input, e.g. "D A wait:500 shot:out.bmp")

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cjson/cJSON.h"
#include "soup/soup_ui.h"
#include "system/keymap_sw.h"

#ifdef PLATFORM_MIYOOMINI
#include "components/kbinput_wrapper.h"
#else
static const char *launch_keyboard(const char *initial, const char *title)
{
    (void)title;
    static char buf[256];
    static int n = 0;
    const char *t = getenv("YT_KEYBOARD_INPUT");
    for (int i = 0; i < n && t; i++)
        if ((t = strchr(t, '|')))
            t++;
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

#define EXIT_PLAY 10
#define MAX_ITEMS 60
#define MAX_INST 24
#define MAX_HISTORY 50
#define ROW_H 78
#define THUMB_W 128
#define THUMB_H 72

typedef struct {
    char id[16], title[200], author[100];
    long length, views;
    int img_state; // 0 none, 1 loading, 2 ready, 3 failed
    SDL_Surface *thumb;
} Video;

typedef struct {
    const char *name;
    Video items[MAX_ITEMS];
    int count, sel, scroll;
    bool loaded;
    char error[160];
} Tab;

enum { T_POPULAR,
       T_SEARCH,
       T_SAVED,
       T_HISTORY,
       T_COUNT };
static Tab tabs[T_COUNT] = {{"Populært"}, {"Søk"}, {"Lagret"}, {"Historikk"}};
static int cur = T_POPULAR;

static char sdcard[256], appdir[300], font_path[300], curl_bin[300], ffmpeg_bin[300];
static char save_dir[320], thumb_dir[340], config_path[400], history_path[400];
static char instances[MAX_INST][120];
static int inst_count = 0, inst_cur = 0;
static char query[200] = "";
static bool quit = false;
static char message[200] = "";
static Uint32 message_until = 0, confirm_until = 0;
static pid_t img_pid = 0;
static Video *img_video = NULL;
static char *script = NULL, *spos = NULL;
static Uint32 swait = 0;

/* ---------- helpers ---------- */

static void show_message(const char *t, Uint32 ms)
{
    copy_str(message, sizeof(message), t);
    message_until = SDL_GetTicks() + ms;
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

static void url_encode(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = c;
        else
            o += snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = '\0';
}

// Tabs and newlines would break the tab-separated files
static void clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static bool valid_id(const char *id)
{
    if (!*id || strlen(id) > 15)
        return false;
    for (; *id; id++)
        if (!isalnum((unsigned char)*id) && *id != '-' && *id != '_')
            return false;
    return true;
}

static void fmt_length(long s, char *out, size_t n)
{
    if (s <= 0)
        *out = '\0';
    else if (s >= 3600)
        snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

static void fmt_views(long v, char *out, size_t n)
{
    if (v >= 1000000)
        snprintf(out, n, "%.1f mill. visninger", v / 1e6);
    else if (v >= 1000)
        snprintf(out, n, "%ld k visninger", v / 1000);
    else if (v > 0)
        snprintf(out, n, "%ld visninger", v);
    else
        *out = '\0';
}

/* ---------- HTTP via curl ---------- */

// GET a URL and return the body (malloc'd), or NULL
static char *http_get(const char *url, int timeout_s)
{
    char tmo[16];
    snprintf(tmo, sizeof(tmo), "%d", timeout_s);
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0)
            dup2(dn, 2);
        execlp(curl_bin, curl_bin, "-s", "-k", "-L", "-f", "-m", tmo, "-A", "Mozilla/5.0 OnionSoup", url,
               (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    ssize_t r;
    while (buf && (r = read(fds[0], buf + len, cap - len - 1)) > 0) {
        len += r;
        if (cap - len < 8192) {
            char *nb = realloc(buf, cap *= 2);
            if (!nb) {
                free(buf);
                buf = NULL;
            }
            buf = nb;
        }
    }
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!buf)
        return NULL;
    buf[len] = '\0';
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ---------- Invidious servers ---------- */

static void add_instance(const char *u)
{
    if (inst_count >= MAX_INST || strncmp(u, "https://", 8))
        return;
    for (int i = 0; i < inst_count; i++)
        if (!strcmp(instances[i], u))
            return;
    copy_str(instances[inst_count], sizeof(instances[0]), u);
    size_t l = strlen(instances[inst_count]);
    while (l && instances[inst_count][l - 1] == '/')
        instances[inst_count][--l] = '\0';
    inst_count++;
}

static void load_instances(void)
{
    char path[400], line[200];
    // the last server that worked goes first
    read_file_line(config_path, line, sizeof(line));
    if (*line)
        add_instance(line);
    snprintf(path, sizeof(path), "%s/instances.txt", appdir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n \t#")] = '\0';
            add_instance(line);
        }
        fclose(f);
    }
    if (!inst_count)
        add_instance("https://inv.nadeko.net");
}

static void save_instance(void)
{
    FILE *f = fopen(config_path, "w");
    if (f) {
        fprintf(f, "%s\n", instances[inst_cur]);
        fclose(f);
    }
}

// Add servers from the public list that have the API turned on
static bool fetch_public_instances(void)
{
    char *res = http_get("https://api.invidious.io/instances.json?sort_by=health", 15);
    cJSON *j = res ? cJSON_Parse(res) : NULL;
    free(res);
    int before = inst_count;
    cJSON *e;
    cJSON_ArrayForEach(e, j)
    {
        cJSON *info = cJSON_GetArrayItem(e, 1);
        cJSON *uri = cJSON_GetObjectItem(info, "uri");
        if (cJSON_IsTrue(cJSON_GetObjectItem(info, "api")) && cJSON_IsString(uri))
            add_instance(uri->valuestring);
    }
    cJSON_Delete(j);
    return inst_count > before;
}

static void draw(void);

// GET an API path, trying every server until one answers with JSON
static cJSON *api(const char *path)
{
    static bool fetched_public = false;
    for (int round = 0; round < 2; round++) {
        for (int k = 0; k < inst_count; k++) {
            int i = (inst_cur + k) % inst_count;
            char url[600];
            snprintf(url, sizeof(url), "%s%s", instances[i], path);
            char *res = http_get(url, 12);
            cJSON *j = res ? cJSON_Parse(res) : NULL;
            free(res);
            if (j && !cJSON_GetObjectItem(j, "error")) {
                if (i != inst_cur || k) {
                    inst_cur = i;
                    save_instance();
                }
                return j;
            }
            cJSON_Delete(j);
        }
        if (fetched_public || !fetch_public_instances())
            break;
        fetched_public = true;
    }
    return NULL;
}

/* ---------- lists ---------- */

static void free_tab(Tab *t)
{
    for (int i = 0; i < t->count; i++)
        if (t->items[i].thumb)
            SDL_FreeSurface(t->items[i].thumb);
    if (img_video >= t->items && img_video < t->items + MAX_ITEMS)
        img_video = NULL;
    t->count = t->sel = t->scroll = 0;
    t->error[0] = '\0';
}

static Video *add_video(Tab *t, const char *id, const char *title, const char *author, long length, long views)
{
    if (t->count >= MAX_ITEMS || !valid_id(id))
        return NULL;
    Video *v = &t->items[t->count++];
    memset(v, 0, sizeof(*v));
    copy_str(v->id, sizeof(v->id), id);
    copy_str(v->title, sizeof(v->title), title);
    copy_str(v->author, sizeof(v->author), author);
    clean(v->title);
    clean(v->author);
    v->length = length;
    v->views = views;
    return v;
}

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static long jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (long)v->valuedouble : 0;
}

static void fill_from_json(Tab *t, cJSON *arr)
{
    cJSON *o;
    cJSON_ArrayForEach(o, arr)
    {
        const char *type = jstr(o, "type");
        if (*type && strcmp(type, "video") && strcmp(type, "shortVideo"))
            continue;
        if (cJSON_IsTrue(cJSON_GetObjectItem(o, "liveNow")))
            continue; // live streams are not MP4
        add_video(t, jstr(o, "videoId"), jstr(o, "title"), jstr(o, "author"), jnum(o, "lengthSeconds"),
                  jnum(o, "viewCount"));
    }
}

static void busy(const char *what)
{
    show_message(what, 60000);
    draw();
}

static void load_popular(void)
{
    Tab *t = &tabs[T_POPULAR];
    free_tab(t);
    busy("Henter populære videoer …");
    cJSON *j = api("/api/v1/trending?region=NO");
    if (!j || !cJSON_GetArraySize(j)) {
        cJSON_Delete(j);
        j = api("/api/v1/popular");
    }
    fill_from_json(t, j);
    cJSON_Delete(j);
    if (!t->count)
        copy_str(t->error, sizeof(t->error), "Fant ingen Invidious-server som svarer. Sjekk WiFi, eller trykk SELECT for å prøve en annen server.");
    t->loaded = true;
    message_until = 0;
}

static void do_search(void)
{
    const char *r = launch_keyboard(query, "Søk på YouTube");
    if (!r)
        return;
    char q[200];
    copy_str(q, sizeof(q), r);
    // trim
    char *s = q;
    while (*s == ' ')
        s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\n' || s[l - 1] == '\r'))
        s[--l] = '\0';
    if (!*s)
        return;
    copy_str(query, sizeof(query), s);
    Tab *t = &tabs[T_SEARCH];
    free_tab(t);
    cur = T_SEARCH;
    busy("Søker …");
    char enc[600], path[700];
    url_encode(query, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/api/v1/search?type=video&q=%s", enc);
    cJSON *j = api(path);
    fill_from_json(t, j);
    if (!j)
        copy_str(t->error, sizeof(t->error), "Søket feilet. Sjekk WiFi, eller trykk SELECT for å prøve en annen server.");
    else if (!t->count)
        snprintf(t->error, sizeof(t->error), "Ingen treff for «%s».", query);
    cJSON_Delete(j);
    t->loaded = true;
    message_until = 0;
}

// Saved videos: SDCARD/YouTube/<id>.mp4 with <id>.txt (title, author, length)
static void read_meta(const char *path, Tab *t, const char *id)
{
    char line[400] = "";
    read_file_line(path, line, sizeof(line));
    char *f[3] = {line, "", "0"};
    for (int i = 1; i < 3; i++) {
        char *tab = strchr(f[i - 1], '\t');
        if (!tab)
            break;
        *tab = '\0';
        f[i] = tab + 1;
    }
    add_video(t, id, *f[0] ? f[0] : id, f[1], atol(f[2]), 0);
}

static void load_saved(void)
{
    Tab *t = &tabs[T_SAVED];
    int sel = t->sel;
    free_tab(t);
    DIR *d = opendir(save_dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".txt"))
            continue;
        char id[16], path[700];
        snprintf(id, sizeof(id), "%.*s", (int)(l - 4), e->d_name);
        snprintf(path, sizeof(path), "%s/%s", save_dir, e->d_name);
        read_meta(path, t, id);
    }
    if (d)
        closedir(d);
    t->sel = sel < t->count ? sel : (t->count ? t->count - 1 : 0);
    if (!t->count)
        copy_str(t->error, sizeof(t->error), "Ingen lagrede videoer ennå. Trykk X på en video for å laste den ned.");
    t->loaded = true;
}

static void load_history(void)
{
    Tab *t = &tabs[T_HISTORY];
    free_tab(t);
    FILE *f = fopen(history_path, "r");
    char line[500];
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p[4] = {line, "", "", "0"};
        for (int i = 1; i < 4; i++) {
            char *tab = strchr(p[i - 1], '\t');
            if (!tab)
                break;
            *tab = '\0';
            p[i] = tab + 1;
        }
        add_video(t, p[0], p[1], p[2], atol(p[3]), 0);
    }
    if (f)
        fclose(f);
    if (!t->count)
        copy_str(t->error, sizeof(t->error), "Videoer du ser på havner her.");
    t->loaded = true;
}

static void add_history(Video *v)
{
    Video copy = *v;
    char *old = read_all(history_path);
    FILE *f = fopen(history_path, "w");
    if (!f) {
        free(old);
        return;
    }
    fprintf(f, "%s\t%s\t%s\t%ld\n", copy.id, copy.title, copy.author, copy.length);
    int n = 1;
    for (char *line = old ? strtok(old, "\n") : NULL; line && n < MAX_HISTORY; line = strtok(NULL, "\n")) {
        size_t l = strlen(copy.id);
        if (!strncmp(line, copy.id, l) && line[l] == '\t')
            continue;
        fprintf(f, "%s\n", line);
        n++;
    }
    fclose(f);
    free(old);
    tabs[T_HISTORY].loaded = false;
}

static void open_tab(int i)
{
    cur = i;
    Tab *t = &tabs[i];
    if (i == T_SEARCH && !t->count)
        t->sel = -1; // the search field
    if (i == T_SAVED)
        load_saved();
    else if (i == T_HISTORY && !t->loaded)
        load_history();
    else if (i == T_POPULAR && !t->loaded)
        load_popular();
}

/* ---------- downloads ---------- */

static void saved_path(const char *id, const char *ext, char *out, size_t n)
{
    snprintf(out, n, "%s/%s%s", save_dir, id, ext);
}

static int saved_state(const char *id) // 0 no, 1 downloading, 2 done
{
    char p[400];
    saved_path(id, ".mp4", p, sizeof(p));
    if (access(p, F_OK) == 0)
        return 2;
    saved_path(id, ".mp4.part", p, sizeof(p));
    return access(p, F_OK) == 0 ? 1 : 0;
}

static void yt_log(const char *fmt, ...)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/youtube.log", save_dir);
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

// Does the URL deliver video data? Fetches the first 2 KB.
static bool probe(const char *url)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1);
        dup2(dn, 2);
        execlp(curl_bin, curl_bin, "-s", "-k", "-L", "-f", "-m", "15", "-r", "0-2047", "-A", "Mozilla/5.0 OnionSoup",
               "-o", "/dev/null", url, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (pid > 0)
        waitpid(pid, &st, 0);
    return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// Find a playable MP4 URL (360p, itag 18) on one of the servers. Servers
// often answer search but cannot play (YouTube blocks them), so every
// candidate is tested before it is used.
static bool resolve_stream(const char *id, char *out, size_t n)
{
    busy("Finner videoen …");
    for (int k = 0; k < inst_count; k++) {
        int i = (inst_cur + k) % inst_count;
        const char *base = instances[i];
        char url[1600], path[400];
        snprintf(path, sizeof(path), "%s/api/v1/videos/%s?local=true&fields=formatStreams", base, id);
        char *res = http_get(path, 15);
        cJSON *j = res ? cJSON_Parse(res) : NULL;
        free(res);
        cJSON *fs = cJSON_GetObjectItem(j, "formatStreams"), *o, *pick = NULL;
        cJSON_ArrayForEach(o, fs)
        {
            cJSON *itag = cJSON_GetObjectItem(o, "itag");
            bool is18 = (cJSON_IsString(itag) && !strcmp(itag->valuestring, "18")) ||
                        (cJSON_IsNumber(itag) && itag->valueint == 18);
            if (is18 || (!pick && strstr(jstr(o, "type"), "mp4")))
                pick = o;
            if (is18)
                break;
        }
        const char *u = pick ? jstr(pick, "url") : "";
        bool ok = false;
        if (*u) {
            snprintf(url, sizeof(url), "%s%s", *u == '/' ? base : "", u);
            ok = probe(url);
            yt_log("%s: api url %s", base, ok ? "ok" : "failed");
        }
        else
            yt_log("%s: no formatStreams", base);
        cJSON_Delete(j);
        if (!ok) {
            snprintf(url, sizeof(url), "%s/latest_version?id=%s&itag=18&local=true", base, id);
            ok = probe(url);
            yt_log("%s: latest_version %s", base, ok ? "ok" : "failed");
        }
        if (ok) {
            if (i != inst_cur) {
                inst_cur = i;
                save_instance();
            }
            copy_str(out, n, url);
            message_until = 0;
            return true;
        }
    }
    show_message("Ingen server kunne spille videoen. Prøv igjen senere.", 4000);
    return false;
}

static void download(Video *v)
{
    if (saved_state(v->id)) {
        show_message(saved_state(v->id) == 2 ? "Allerede lagret" : "Lastes allerede ned", 2000);
        return;
    }
    char meta[400], part[400], done[400], url[1600];
    saved_path(v->id, ".txt", meta, sizeof(meta));
    saved_path(v->id, ".mp4.part", part, sizeof(part));
    saved_path(v->id, ".mp4", done, sizeof(done));
    if (!resolve_stream(v->id, url, sizeof(url)))
        return;
    FILE *f = fopen(meta, "w");
    if (!f) {
        show_message("Kunne ikke lagre på minnekortet", 3000);
        return;
    }
    fprintf(f, "%s\t%s\t%ld\n", v->title, v->author, v->length);
    fclose(f);
    close(open(part, O_WRONLY | O_CREAT, 0644));
    // the download keeps going after the app exits (double fork)
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int dn = open("/dev/null", O_RDWR);
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
            execl("/bin/sh", "sh", "-c",
                  "\"$0\" -s -k -L -f --retry 3 -m 21600 -o \"$1\" \"$2\" && mv \"$1\" \"$3\" || "
                  "{ rm -f \"$1\" \"$4\"; }",
                  curl_bin, part, url, done, meta, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    show_message("Laster ned – finnes snart under Lagret", 2500);
}

static void delete_saved(Video *v)
{
    char p[400];
    const char *exts[] = {".mp4", ".mp4.part", ".txt"};
    for (int i = 0; i < 3; i++) {
        saved_path(v->id, exts[i], p, sizeof(p));
        remove(p);
    }
    show_message("Slettet", 1500);
    load_saved();
}

/* ---------- thumbnails ---------- */

static void thumb_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/%s.png", thumb_dir, id); }

static void load_thumb(Video *v)
{
    char p[400];
    thumb_path(v->id, p, sizeof(p));
    SDL_Surface *img = IMG_Load(p);
    if (!img) {
        v->img_state = 3;
        return;
    }
    v->thumb = scale_rounded(img, THUMB_W, THUMB_H, 8);
    SDL_FreeSurface(img);
    v->img_state = v->thumb ? 2 : 3;
}

static void request_thumb(Video *v)
{
    if (v->img_state)
        return;
    char p[400];
    thumb_path(v->id, p, sizeof(p));
    if (access(p, R_OK) == 0) {
        load_thumb(v);
        return;
    }
    if (img_pid > 0)
        return;
    char url[200];
    snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", v->id);
    v->img_state = 1;
    img_video = v;
    img_pid = fork();
    if (img_pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1);
        dup2(dn, 2);
        // JPEG -> small PNG (the Miyoo's SDL_image only reads PNG)
        execl("/bin/sh", "sh", "-c",
              "\"$0\" -s -k -f -m 15 -o \"$2.jpg\" \"$3\" && \"$1\" -y -loglevel quiet -i \"$2.jpg\" "
              "-vf scale=128:72 \"$2.tmp.png\" && mv \"$2.tmp.png\" \"$2\"; r=$?; rm -f \"$2.jpg\" \"$2.tmp.png\"; "
              "exit $r",
              curl_bin, ffmpeg_bin, p, url, (char *)NULL);
        _exit(127);
    }
}

static void poll_thumbs(void)
{
    if (img_pid > 0) {
        int st;
        if (waitpid(img_pid, &st, WNOHANG) != img_pid)
            return;
        img_pid = 0;
        if (img_video) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                load_thumb(img_video);
            else
                img_video->img_state = 3;
        }
        img_video = NULL;
    }
    // next visible thumbnail without a picture
    Tab *t = &tabs[cur];
    for (int i = t->scroll; i < t->count && i < t->scroll + 6; i++)
        if (!t->items[i].img_state) {
            request_thumb(&t->items[i]);
            if (img_pid > 0)
                break;
        }
}

/* ---------- playing ---------- */

// Remember the tab and the list, so the app comes back to the same place
static void save_state(void)
{
    FILE *f = fopen("/tmp/youtube_state", "w");
    if (!f)
        return;
    Tab *t = &tabs[cur];
    fprintf(f, "%d\t%d\t%d\t%s\n", cur, t->sel, t->scroll, query);
    for (int k = 0; k < 2; k++) {
        Tab *s = &tabs[k == 0 ? T_POPULAR : T_SEARCH];
        for (int i = 0; i < s->count; i++)
            fprintf(f, "%d\t%s\t%s\t%s\t%ld\t%ld\n", k == 0 ? T_POPULAR : T_SEARCH, s->items[i].id,
                    s->items[i].title, s->items[i].author, s->items[i].length, s->items[i].views);
    }
    fclose(f);
}

static bool restore_state(void)
{
    FILE *f = fopen("/tmp/youtube_state", "r");
    if (!f)
        return false;
    char line[700];
    int tab = T_POPULAR, sel = 0, scroll = 0;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *q = line;
        for (int i = 0; i < 3 && q; i++)
            if ((q = strchr(q, '\t')))
                q++;
        if (q)
            copy_str(query, sizeof(query), q);
        sscanf(line, "%d\t%d\t%d", &tab, &sel, &scroll);
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p[6] = {line, "", "", "", "0", "0"};
        for (int i = 1; i < 6; i++) {
            char *t = strchr(p[i - 1], '\t');
            if (!t)
                break;
            *t = '\0';
            p[i] = t + 1;
        }
        int k = atoi(p[0]);
        if (k == T_POPULAR || k == T_SEARCH) {
            add_video(&tabs[k], p[1], p[2], p[3], atol(p[4]), atol(p[5]));
            tabs[k].loaded = true;
        }
    }
    fclose(f);
    if (tab < 0 || tab >= T_COUNT)
        tab = T_POPULAR;
    open_tab(tab);
    Tab *t = &tabs[cur];
    if (sel < t->count) {
        t->sel = sel;
        t->scroll = scroll <= sel ? scroll : sel;
    }
    return true;
}

static void play(Video *v)
{
    char p[400], url[1600] = "";
    saved_path(v->id, ".mp4", p, sizeof(p));
    bool local = access(p, R_OK) == 0;
    if (!local && !resolve_stream(v->id, url, sizeof(url)))
        return;
    FILE *f = fopen("/tmp/youtube_play", "w");
    if (!f) {
        show_message("Kunne ikke starte avspilling", 3000);
        return;
    }
    fprintf(f, "title=%s\nduration=%ld\n", v->title, v->length);
    if (local)
        fprintf(f, "url=%s\nlocal=1\n", p);
    else
        fprintf(f, "stream=%s\n", url);
    fclose(f);
    add_history(v);
    save_state();
    quit = true;
}

/* ---------- drawing ---------- */

static void draw_header(void)
{
    fill(0, 0, SCREEN_W, TOP_H, C_BAR);
    text(f24, "You", 18, 10, C_ACCENT);
    text(f24, "Tube", 18 + text_w(f24, "You"), 10, C_WHITE);
    int x = SCREEN_W - 12;
    for (int i = T_COUNT - 1; i >= 0; i--) {
        int w = text_w(f17, tabs[i].name);
        x -= w + 10;
        if (i == cur) {
            fill_round(x - 10, 12, w + 20, 28, 14, C_ACCENT);
            text(f17, tabs[i].name, x, 15, C_DARK);
        }
        else
            text(f17, tabs[i].name, x, 15, C_GREY);
        x -= 16;
    }
}

static void draw_list(void)
{
    Tab *t = &tabs[cur];
    int y0 = TOP_H + 8;
    if (cur == T_SEARCH) {
        // the search field
        bool s = t->sel < 0;
        outline_round(16, y0, SCREEN_W - 32, 40, 20, 2, s ? C_ACCENT : C_PANEL, C_PANEL);
        text_fit(f17, *query ? query : "Trykk A eller START for å søke …", 34, y0 + 9, SCREEN_W - 80,
                 *query ? C_WHITE : C_GREY);
        y0 += 50;
    }
    int visible = (BOTTOM_Y - y0) / ROW_H;
    if (!t->count) {
        if (*t->error)
            text_wrap(f17, t->error, 60, y0 + 90, SCREEN_W - 120, 24, 5, C_GREY);
        return;
    }
    if (t->sel >= 0 && t->sel < t->scroll)
        t->scroll = t->sel;
    if (t->sel >= t->scroll + visible)
        t->scroll = t->sel - visible + 1;
    for (int k = 0; k < visible && t->scroll + k < t->count; k++) {
        int i = t->scroll + k;
        Video *v = &t->items[i];
        int y = y0 + k * ROW_H;
        bool s = i == t->sel;
        if (s)
            fill_round(12, y, SCREEN_W - 24, ROW_H - 4, 12, C_CARD);
        int tx = 20, ty = y + 1;
        if (v->img_state == 2 && v->thumb) {
            SDL_Rect pos = {tx, ty};
            SDL_BlitSurface(v->thumb, NULL, screen, &pos);
        }
        else
            fill_round(tx, ty, THUMB_W, THUMB_H, 8, C_PANEL);
        char len[24];
        fmt_length(v->length, len, sizeof(len));
        if (*len) {
            int w = text_w(f13, len) + 10;
            fill_round(tx + THUMB_W - w - 4, ty + THUMB_H - 22, w, 18, 6, C_DARK);
            text(f13, len, tx + THUMB_W - w + 1, ty + THUMB_H - 22, C_WHITE);
        }
        int x = tx + THUMB_W + 14, maxw = SCREEN_W - x - 24;
        text_wrap(f17, v->title, x, y + 4, maxw, 22, 2, s ? C_WHITE : C_SOFT);
        char sub[200], views[48];
        fmt_views(v->views, views, sizeof(views));
        snprintf(sub, sizeof(sub), "%s%s%s", v->author, *views && *v->author ? "  ·  " : "", views);
        int st = saved_state(v->id);
        if (st && cur != T_SAVED) {
            const char *b = st == 2 ? "LAGRET" : "LASTER NED";
            text(f13, b, SCREEN_W - 32 - text_w(f13, b), y + 52, C_ACCENT);
            maxw -= text_w(f13, b) + 16;
        }
        else if (st == 1)
            copy_str(sub, sizeof(sub), "Laster ned …");
        text_fit(f15, sub, x, y + 50, maxw, C_GREY);
    }
}

static void draw(void)
{
    SDL_FillRect(screen, NULL, map(screen, C_BG));
    draw_header();
    draw_list();
    Tab *t = &tabs[cur];
    const char *k[6], *l[6];
    int n = 0;
#define HINT(a, b) (k[n] = (a), l[n] = (b), n++)
    if (t->count && t->sel >= 0) {
        HINT("A", "Spill av");
        if (cur == T_SAVED)
            HINT("Y", "Slett");
        else if (!saved_state(t->items[t->sel].id))
            HINT("X", "Last ned");
    }
    else if (cur == T_SEARCH)
        HINT("A", "Søk");
    HINT("L/R", "Fane");
    HINT("B", "Avslutt");
    if (cur != T_SAVED)
        HINT("SELECT", "Server");
#undef HINT
    draw_hints(k, l, n);
    if (*message && SDL_GetTicks() < message_until) {
        int w = text_w(f17, message) + 40;
        if (w > SCREEN_W - 40)
            w = SCREEN_W - 40;
        fill_round((SCREEN_W - w) / 2, BOTTOM_Y - 60, w, 40, 20, C_PANEL);
        text_fit(f17, message, (SCREEN_W - w) / 2 + 20, BOTTOM_Y - 52, w - 40, C_WHITE);
    }
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

/* ---------- input ---------- */

static void on_key(SDLKey key)
{
    Tab *t = &tabs[cur];
    int top = cur == T_SEARCH ? -1 : 0; // -1 is the search field
    if (key != SW_BTN_Y)
        confirm_until = 0;
    switch (key) {
    case SW_BTN_B:
    case SW_BTN_MENU:
        quit = true;
        break;
    case SW_BTN_L1:
    case SW_BTN_R1:
        open_tab((cur + (key == SW_BTN_R1 ? 1 : T_COUNT - 1)) % T_COUNT);
        break;
    case SW_BTN_UP:
        if (t->sel > top)
            t->sel--;
        break;
    case SW_BTN_DOWN:
        if (t->sel < t->count - 1)
            t->sel++;
        break;
    case SW_BTN_LEFT:
        t->sel = t->sel - 5 > top ? t->sel - 5 : top;
        break;
    case SW_BTN_RIGHT:
        if (t->count)
            t->sel = t->sel + 5 < t->count ? t->sel + 5 : t->count - 1;
        break;
    case SW_BTN_START:
        do_search();
        break;
    case SW_BTN_A:
        if (t->sel < 0 || (cur == T_SEARCH && !t->count))
            do_search();
        else if (t->count)
            play(&t->items[t->sel]);
        break;
    case SW_BTN_X:
        if (t->count && t->sel >= 0 && cur != T_SAVED)
            download(&t->items[t->sel]);
        break;
    case SW_BTN_Y:
        if (cur == T_SAVED && t->count) {
            if (SDL_GetTicks() < confirm_until) {
                confirm_until = 0;
                delete_saved(&t->items[t->sel]);
            }
            else {
                confirm_until = SDL_GetTicks() + 3000;
                show_message("Trykk Y igjen for å slette", 3000);
            }
        }
        break;
    case SW_BTN_SELECT:
        if (cur == T_SAVED || inst_count < 1)
            break;
        inst_cur = (inst_cur + 1) % inst_count;
        save_instance();
        {
            char m[200];
            snprintf(m, sizeof(m), "Server: %s", instances[inst_cur] + 8);
            show_message(m, 2500);
        }
        if (cur == T_POPULAR)
            load_popular();
        break;
    default:
        break;
    }
}

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
    if ((e = getenv("YT_DIR")))
        copy_str(appdir, sizeof(appdir), e);
    else
        snprintf(appdir, sizeof(appdir), "%s/App/YouTube", sdcard);
    if ((e = getenv("YT_FONT")))
        copy_str(font_path, sizeof(font_path), e);
    else
        snprintf(font_path, sizeof(font_path), "%s/miyoo/app/Exo-2-Bold-Italic_Universal.ttf", sdcard);
    copy_str(curl_bin, sizeof(curl_bin), (e = getenv("YT_CURL")) ? e : "curl");
    if ((e = getenv("YT_FFMPEG")))
        copy_str(ffmpeg_bin, sizeof(ffmpeg_bin), e);
    else
        snprintf(ffmpeg_bin, sizeof(ffmpeg_bin), "%s/.tmp_update/bin/ffmpeg", sdcard);
    if ((e = getenv("YT_SCRIPT")))
        spos = script = strdup(e);
    snprintf(save_dir, sizeof(save_dir), "%s/YouTube", sdcard);
    snprintf(thumb_dir, sizeof(thumb_dir), "%s/.thumbs", save_dir);
    char cfg[300];
    snprintf(cfg, sizeof(cfg), "%s/Saves/CurrentProfile/config", sdcard);
    mkdir_p(cfg);
    mkdir_p(thumb_dir);
    snprintf(config_path, sizeof(config_path), "%s/youtube_server.txt", cfg);
    snprintf(history_path, sizeof(history_path), "%s/youtube_history.txt", cfg);
    remove("/tmp/youtube_play");
    load_instances();

    SDL_Init(SDL_INIT_VIDEO);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(300, 60);
    TTF_Init();
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0, 0, 0, 0);
    if (!soup_open_fonts(font_path)) {
        fprintf(stderr, "youtube: cannot open font %s\n", font_path);
        return 1;
    }

    if (!restore_state())
        open_tab(T_POPULAR);
    remove("/tmp/youtube_state");

    // Redraw at once after input, otherwise ~8 times a second: a full redraw
    // every frame keeps the CPU busy and slows down background downloads
    // After a few seconds without input: one redraw a second and longer
    // sleeps, so the CPU can clock down (battery)
    Uint32 last_draw = 0, last_input = 0;
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
        poll_thumbs();
        Uint32 now = SDL_GetTicks();
        if (dirty)
            last_input = now;
        bool idle = now - last_input > 3000 && !(img_pid > 0);
        if (dirty || now - last_draw >= (idle ? 1000 : 120)) {
            draw();
            last_draw = now;
        }
        SDL_Delay(idle ? 50 : 20);
    }

    int code = access("/tmp/youtube_play", F_OK) == 0 ? EXIT_PLAY : 0;
    if (img_pid > 0)
        waitpid(img_pid, NULL, 0);
    free(script);
    TTF_Quit();
    SDL_Quit();
    return code;
}

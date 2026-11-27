#define _GNU_SOURCE
#include "book.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TXT_CHUNK 48000

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

bool book_supported(const char *name)
{
    const char *e = ext_of(name);
    return name[0] != '.' && (!strcasecmp(e, "epub") || !strcasecmp(e, "txt"));
}

void book_guess_from_name(const char *file, char *title, size_t tn, char *author, size_t an)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    char name[256];
    copy(name, sizeof(name), base);
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = '\0';
    for (char *p = name; *p; p++)
        if (*p == '_')
            *p = ' ';
    author[0] = '\0';
    char *dash = strstr(name, " - ");
    if (dash) {
        *dash = '\0';
        copy(author, an, name);
        copy(title, tn, dash + 3);
    }
    else
        copy(title, tn, name);
}

/* ---------- text encoding ---------- */

static bool valid_utf8(const unsigned char *s, long n)
{
    long i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2
                             : (c >> 4) == 14  ? 3
                             : (c >> 3) == 30  ? 4
                                               : 0;
        if (!len || i + len > n)
            return false;
        for (int k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
        i += len;
    }
    return true;
}

// Windows-1252 (a superset of Latin-1 for printable text) to UTF-8
static char *cp1252_to_utf8(const unsigned char *s, long n, long *out_len)
{
    static const unsigned short hi[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    char *out = malloc(n * 3 + 1);
    if (!out)
        return NULL;
    long o = 0;
    for (long i = 0; i < n; i++) {
        unsigned cp = s[i];
        if (cp >= 0x80 && cp < 0xA0)
            cp = hi[cp - 0x80];
        o += utf8_encode(cp, out + o);
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

static char *read_file(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (buf) {
        n = (long)fread(buf, 1, n, fp);
        buf[n] = '\0';
    }
    fclose(fp);
    *len = n;
    return buf;
}

/* ---------- EPUB ---------- */

typedef struct {
    char id[128], href[512], media[64], props[96];
} Item;

static void dir_of(const char *path, char *out, size_t n)
{
    copy(out, n, path);
    char *slash = strrchr(out, '/');
    if (slash)
        *slash = '\0';
    else
        out[0] = '\0';
}

static char *strip_tags(const char *s)
{
    // descriptions are often escaped HTML: decode, drop the tags, decode again
    size_t n = strlen(s) + 1;
    char *a = malloc(n), *b = malloc(n);
    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    xml_text(s, s + strlen(s), a, n);
    size_t o = 0;
    bool in_tag = false;
    for (char *p = a; *p; p++) {
        if (*p == '<')
            in_tag = true;
        else if (*p == '>') {
            in_tag = false;
            b[o++] = ' ';
        }
        else if (!in_tag)
            b[o++] = *p;
    }
    b[o] = '\0';
    xml_text(b, b + o, a, n);
    free(b);
    return a;
}

static int chapter_of(Book *b, const char *path)
{
    for (int i = 0; i < b->nchapters; i++)
        if (!strcmp(b->chapters[i].path, path))
            return i;
    for (int i = 0; i < b->nchapters; i++)
        if (!strcasecmp(b->chapters[i].path, path))
            return i;
    return -1;
}

static void add_toc(Book *b, const char *label, int chapter)
{
    if (chapter < 0 || !*label)
        return;
    // keep only the first entry per chapter (sub-sections point into the same file)
    for (int i = 0; i < b->ntoc; i++)
        if (b->toc[i].chapter == chapter)
            return;
    b->toc = realloc(b->toc, (b->ntoc + 1) * sizeof(TocEntry));
    copy(b->toc[b->ntoc].title, sizeof(b->toc[0].title), label);
    b->toc[b->ntoc].chapter = chapter;
    b->ntoc++;
}

static void parse_ncx(Book *b, const char *path)
{
    char *x = zip_read_name(b->zip, path, NULL);
    if (!x)
        return;
    char dir[512];
    dir_of(path, dir, sizeof(dir));
    char label[160] = "";
    const char *p = x;
    XTag t;
    while (xml_next(&p, &t, NULL)) {
        if (t.end || t.name[0] == '!')
            continue;
        if (!strcmp(t.name, "text"))
            xml_inner_text(&t, label, sizeof(label));
        else if (!strcmp(t.name, "content")) {
            char src[512], full[1024];
            if (xml_attr(&t, "src", src, sizeof(src))) {
                path_join(dir, src, full, sizeof(full));
                add_toc(b, label, chapter_of(b, full));
            }
            label[0] = '\0';
        }
    }
    free(x);
}

static void parse_nav(Book *b, const char *path)
{
    char *x = zip_read_name(b->zip, path, NULL);
    if (!x)
        return;
    char dir[512];
    dir_of(path, dir, sizeof(dir));
    const char *p = x;
    XTag t;
    bool in_toc = false;
    while (xml_next(&p, &t, NULL)) {
        if (!strcmp(t.name, "nav")) {
            if (t.end) {
                if (in_toc)
                    break;
                continue;
            }
            char type[64];
            in_toc = xml_attr(&t, "type", type, sizeof(type)) && strstr(type, "toc");
        }
        else if (in_toc && !t.end && !strcmp(t.name, "a")) {
            char href[512], full[1024], label[160];
            if (xml_attr(&t, "href", href, sizeof(href))) {
                xml_inner_text(&t, label, sizeof(label));
                path_join(dir, href, full, sizeof(full));
                add_toc(b, label, chapter_of(b, full));
            }
        }
    }
    free(x);
}

static bool is_image(const char *media) { return !strncmp(media, "image/", 6); }

static void first_image_of(Book *b, const char *chapter_path, char *out, size_t n)
{
    char *x = zip_read_name(b->zip, chapter_path, NULL);
    if (!x)
        return;
    char dir[512];
    dir_of(chapter_path, dir, sizeof(dir));
    const char *p = x;
    XTag t;
    while (xml_next(&p, &t, NULL)) {
        if (t.end || (strcmp(t.name, "img") && strcmp(t.name, "image")))
            continue;
        char src[512];
        if (xml_attr(&t, "src", src, sizeof(src)) || xml_attr(&t, "href", src, sizeof(src))) {
            path_join(dir, src, out, n);
            break;
        }
    }
    free(x);
}

static bool open_epub(Book *b, bool meta_only)
{
    b->zip = zip_open(b->file);
    if (!b->zip)
        return false;
    char *container = zip_read_name(b->zip, "META-INF/container.xml", NULL);
    char opf_path[512] = "";
    if (container) {
        const char *p = container;
        XTag t;
        while (xml_next(&p, &t, NULL))
            if (!t.end && !strcmp(t.name, "rootfile") && xml_attr(&t, "full-path", opf_path, sizeof(opf_path)))
                break;
        free(container);
    }
    if (!*opf_path) {
        // no container: take the first .opf in the archive
        for (int i = 0; i < b->zip->count; i++)
            if (!strcasecmp(ext_of(b->zip->entries[i].name), "opf")) {
                copy(opf_path, sizeof(opf_path), b->zip->entries[i].name);
                break;
            }
    }
    char *opf = zip_read_name(b->zip, opf_path, NULL);
    if (!opf)
        return false;
    char dir[512];
    dir_of(opf_path, dir, sizeof(dir));

    Item *items = NULL;
    int nitems = 0;
    char cover_id[128] = "", toc_id[128] = "";
    char **spine = NULL;
    int nspine = 0;
    const char *p = opf;
    XTag t;
    while (xml_next(&p, &t, NULL)) {
        if (t.end || t.name[0] == '!')
            continue;
        if (!strcmp(t.name, "title") && !*b->title)
            xml_inner_text(&t, b->title, sizeof(b->title));
        else if (!strcmp(t.name, "creator") && !*b->author)
            xml_inner_text(&t, b->author, sizeof(b->author));
        else if (!strcmp(t.name, "description") && !b->description) {
            char *raw = malloc(8192);
            if (raw) {
                xml_inner_text(&t, raw, 8192);
                b->description = strip_tags(raw);
                free(raw);
            }
        }
        else if (!strcmp(t.name, "meta")) {
            char name[32];
            if (xml_attr(&t, "name", name, sizeof(name)) && !strcmp(name, "cover"))
                xml_attr(&t, "content", cover_id, sizeof(cover_id));
        }
        else if (!strcmp(t.name, "item")) {
            items = realloc(items, (nitems + 1) * sizeof(Item));
            Item *it = &items[nitems++];
            char href[512];
            xml_attr(&t, "id", it->id, sizeof(it->id));
            xml_attr(&t, "href", href, sizeof(href));
            path_join(dir, href, it->href, sizeof(it->href));
            xml_attr(&t, "media-type", it->media, sizeof(it->media));
            xml_attr(&t, "properties", it->props, sizeof(it->props));
        }
        else if (!strcmp(t.name, "spine"))
            xml_attr(&t, "toc", toc_id, sizeof(toc_id));
        else if (!strcmp(t.name, "itemref")) {
            char idref[128], linear[8];
            xml_attr(&t, "idref", idref, sizeof(idref));
            if (xml_attr(&t, "linear", linear, sizeof(linear)) && !strcmp(linear, "no"))
                continue;
            spine = realloc(spine, (nspine + 1) * sizeof(char *));
            spine[nspine++] = strdup(idref);
        }
    }
    free(opf);

    // cover image
    for (int i = 0; i < nitems && !*b->cover; i++)
        if (strstr(items[i].props, "cover-image"))
            copy(b->cover, sizeof(b->cover), items[i].href);
    for (int i = 0; i < nitems && !*b->cover && *cover_id; i++)
        if (!strcmp(items[i].id, cover_id) && is_image(items[i].media))
            copy(b->cover, sizeof(b->cover), items[i].href);
    for (int i = 0; i < nitems && !*b->cover; i++)
        if (is_image(items[i].media) && (strcasestr(items[i].id, "cover") || strcasestr(items[i].href, "cover")))
            copy(b->cover, sizeof(b->cover), items[i].href);

    // chapters in reading order
    for (int s = 0; s < nspine; s++) {
        for (int i = 0; i < nitems; i++) {
            if (strcmp(items[i].id, spine[s]))
                continue;
            b->chapters = realloc(b->chapters, (b->nchapters + 1) * sizeof(Chapter));
            Chapter *c = &b->chapters[b->nchapters++];
            memset(c, 0, sizeof(*c));
            copy(c->path, sizeof(c->path), items[i].href);
            ZipEntry *e = zip_find(b->zip, c->path);
            c->weight = e ? e->size : 1;
            if (c->weight < 1)
                c->weight = 1;
            b->total_weight += c->weight;
            break;
        }
        free(spine[s]);
    }
    free(spine);
    if (!*b->cover && b->nchapters)
        first_image_of(b, b->chapters[0].path, b->cover, sizeof(b->cover));

    if (!meta_only) {
        for (int i = 0; i < nitems && !b->ntoc; i++)
            if ((*toc_id && !strcmp(items[i].id, toc_id)) || strstr(items[i].media, "dtbncx"))
                parse_ncx(b, items[i].href);
        for (int i = 0; i < nitems && !b->ntoc; i++)
            if (strstr(items[i].props, "nav"))
                parse_nav(b, items[i].href);
    }
    free(items);
    return b->nchapters > 0;
}

/* ---------- TXT ---------- */

static bool open_txt(Book *b, bool meta_only)
{
    long len = 0;
    char *raw = read_file(b->file, &len);
    if (!raw)
        return false;
    long skip = (len >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3)) ? 3 : 0;
    if (valid_utf8((unsigned char *)raw + skip, len - skip)) {
        memmove(raw, raw + skip, len - skip + 1);
        b->txt = raw;
        b->txt_len = len - skip;
    }
    else {
        b->txt = cp1252_to_utf8((unsigned char *)raw, len, &b->txt_len);
        free(raw);
        if (!b->txt)
            return false;
    }
    // opening lines as the description
    long dl = b->txt_len < 600 ? b->txt_len : 600;
    b->description = malloc(dl + 8);
    xml_text(b->txt, b->txt + dl, b->description, dl + 8);
    // no entity decoding wanted here, but xml_text only touches '&...;' sequences

    // split into chapters at heading lines, and at paragraph breaks when chunks get long
    long start = 0, pos = 0;
    while (pos <= b->txt_len) {
        const char *line = b->txt + pos;
        const char *nl = memchr(line, '\n', b->txt_len - pos);
        long le = nl ? (long)(nl - b->txt) : b->txt_len;
        const char *a = line, *e = b->txt + le;
        while (a < e && isspace((unsigned char)*a))
            a++;
        while (e > a && isspace((unsigned char)e[-1]))
            e--;
        bool heading = txt_heading_line(a, (int)(e - a));
        bool too_long = pos - start > TXT_CHUNK && (a == e || pos - start > TXT_CHUNK * 4 / 3);
        // a short title block before the first heading stays with the first chapter
        bool tiny_front = heading && b->nchapters == 0 && pos - start < 400;
        if ((heading || too_long) && pos > start && !tiny_front) {
            b->chapters = realloc(b->chapters, (b->nchapters + 1) * sizeof(Chapter));
            Chapter *c = &b->chapters[b->nchapters++];
            memset(c, 0, sizeof(*c));
            c->start = start;
            c->end = pos;
            c->weight = pos - start;
            start = pos;
        }
        if (heading && !meta_only) {
            char label[160];
            int l = (int)(e - a) < 159 ? (int)(e - a) : 159;
            memcpy(label, a, l);
            label[l] = '\0';
            b->toc = realloc(b->toc, (b->ntoc + 1) * sizeof(TocEntry));
            copy(b->toc[b->ntoc].title, sizeof(b->toc[0].title), label);
            b->toc[b->ntoc].chapter = b->nchapters;
            b->ntoc++;
        }
        if (!nl)
            break;
        pos = le + 1;
    }
    if (b->txt_len > start || !b->nchapters) {
        b->chapters = realloc(b->chapters, (b->nchapters + 1) * sizeof(Chapter));
        Chapter *c = &b->chapters[b->nchapters++];
        memset(c, 0, sizeof(*c));
        c->start = start;
        c->end = b->txt_len;
        c->weight = b->txt_len - start > 0 ? b->txt_len - start : 1;
    }
    for (int i = 0; i < b->nchapters; i++)
        b->total_weight += b->chapters[i].weight;
    if (meta_only) {
        free(b->txt);
        b->txt = NULL;
    }
    return true;
}

Book *book_open(const char *file, bool meta_only)
{
    Book *b = calloc(1, sizeof(Book));
    copy(b->file, sizeof(b->file), file);
    b->fmt = !strcasecmp(ext_of(file), "epub") ? FMT_EPUB : FMT_TXT;
    bool ok = b->fmt == FMT_EPUB ? open_epub(b, meta_only) : open_txt(b, meta_only);
    char gt[256], ga[256];
    book_guess_from_name(file, gt, sizeof(gt), ga, sizeof(ga));
    if (!*b->title)
        copy(b->title, sizeof(b->title), gt);
    if (!*b->author)
        copy(b->author, sizeof(b->author), ga);
    if (!ok) {
        book_close(b);
        return NULL;
    }
    return b;
}

void book_close(Book *b)
{
    if (!b)
        return;
    zip_close(b->zip);
    free(b->txt);
    free(b->description);
    free(b->chapters);
    free(b->toc);
    free(b);
}

Doc *book_chapter(Book *b, int index)
{
    if (index < 0 || index >= b->nchapters)
        return NULL;
    Chapter *c = &b->chapters[index];
    if (b->fmt == FMT_TXT)
        return b->txt ? doc_from_text(b->txt + c->start, c->end - c->start) : NULL;
    char *html = zip_read_name(b->zip, c->path, NULL);
    if (!html)
        return doc_from_text("", 0);
    char dir[512];
    dir_of(c->path, dir, sizeof(dir));
    Doc *d = doc_from_html(html, dir);
    free(html);
    return d;
}

unsigned char *book_image(Book *b, const char *path, long *len)
{
    if (b->fmt != FMT_EPUB || !path || !*path)
        return NULL;
    return (unsigned char *)zip_read_name(b->zip, path, len);
}

void book_chapter_title(Book *b, int index, char *out, size_t n)
{
    int best = -1;
    for (int i = 0; i < b->ntoc; i++)
        if (b->toc[i].chapter <= index && (best < 0 || b->toc[i].chapter >= b->toc[best].chapter))
            best = i;
    if (best >= 0)
        copy(out, n, b->toc[best].title);
    else
        snprintf(out, n, "Part %d", index + 1);
}

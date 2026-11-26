#define _GNU_SOURCE
#include "markup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------- tag scanner ---------- */

static const char *skip_to(const char *p, const char *end_marker)
{
    const char *e = strstr(p, end_marker);
    return e ? e + strlen(end_marker) : p + strlen(p);
}

bool xml_next(const char **pp, XTag *t, const char **text_end)
{
    const char *p = strchr(*pp, '<');
    if (!p) {
        if (text_end)
            *text_end = *pp + strlen(*pp);
        *pp += strlen(*pp);
        return false;
    }
    if (text_end)
        *text_end = p;
    memset(t, 0, sizeof(*t));
    if (!strncmp(p, "<!--", 4) || !strncmp(p, "<![CDATA[", 9) || p[1] == '?' || p[1] == '!') {
        strcpy(t->name, "!");
        if (!strncmp(p, "<!--", 4))
            t->after = skip_to(p + 4, "-->");
        else if (!strncmp(p, "<![CDATA[", 9))
            t->after = skip_to(p + 9, "]]>");
        else if (p[1] == '?')
            t->after = skip_to(p + 2, "?>");
        else
            t->after = skip_to(p + 2, ">");
        *pp = t->after;
        return true;
    }
    p++;
    if (*p == '/') {
        t->end = true;
        p++;
    }
    const char *ns = p;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/')
        p++;
    // local name only
    const char *colon = NULL;
    for (const char *q = ns; q < p; q++)
        if (*q == ':')
            colon = q;
    if (colon)
        ns = colon + 1;
    int n = 0;
    for (const char *q = ns; q < p && n < (int)sizeof(t->name) - 1; q++)
        t->name[n++] = tolower((unsigned char)*q);
    t->name[n] = '\0';
    t->attrs = p;
    char quote = 0;
    while (*p && (quote || *p != '>')) {
        if (quote) {
            if (*p == quote)
                quote = 0;
        }
        else if (*p == '"' || *p == '\'')
            quote = *p;
        p++;
    }
    t->attrs_len = (int)(p - t->attrs);
    if (p > t->attrs && p[-1] == '/') {
        t->self_closing = true;
        t->attrs_len--;
    }
    t->after = *p ? p + 1 : p;
    *pp = t->after;
    return true;
}

bool xml_attr(const XTag *t, const char *name, char *out, size_t n)
{
    const char *p = t->attrs, *end = t->attrs + t->attrs_len;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p))
            p++;
        const char *ns = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p))
            p++;
        const char *ne = p;
        const char *colon = NULL;
        for (const char *q = ns; q < ne; q++)
            if (*q == ':')
                colon = q;
        const char *local = colon ? colon + 1 : ns;
        while (p < end && isspace((unsigned char)*p))
            p++;
        const char *vs = p, *ve = p;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p))
                p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < end && *p != q)
                    p++;
                ve = p;
                if (p < end)
                    p++;
            }
            else {
                vs = p;
                while (p < end && !isspace((unsigned char)*p))
                    p++;
                ve = p;
            }
        }
        if (ne == ns) {
            p++;
            continue;
        }
        if ((size_t)(ne - local) == strlen(name) && !strncasecmp(local, name, ne - local)) {
            xml_text(vs, ve, out, n);
            return true;
        }
    }
    if (n)
        out[0] = '\0';
    return false;
}

int utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 63);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 63);
        out[2] = 0x80 | (cp & 63);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 63);
    out[2] = 0x80 | ((cp >> 6) & 63);
    out[3] = 0x80 | (cp & 63);
    return 4;
}

static const struct {
    const char *name;
    unsigned cp;
} entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"shy", 0xAD}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"deg", 0xB0}, {"middot", 0xB7}, {"bull", 0x2022}, {"sect", 0xA7}, {"para", 0xB6}, {"times", 0xD7}, {"divide", 0xF7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"aelig", 0xE6}, {"AElig", 0xC6}, {"oslash", 0xF8}, {"Oslash", 0xD8}, {"aring", 0xE5}, {"Aring", 0xC5}, {"auml", 0xE4}, {"Auml", 0xC4}, {"ouml", 0xF6}, {"Ouml", 0xD6}, {"uuml", 0xFC}, {"Uuml", 0xDC}, {"szlig", 0xDF}, {"eacute", 0xE9}, {"Eacute", 0xC9}, {"egrave", 0xE8}, {"Egrave", 0xC8}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"aacute", 0xE1}, {"agrave", 0xE0}, {"acirc", 0xE2}, {"atilde", 0xE3}, {"ccedil", 0xE7}, {"Ccedil", 0xC7}, {"iacute", 0xED}, {"igrave", 0xEC}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1}, {"Ntilde", 0xD1}, {"oacute", 0xF3}, {"ograve", 0xF2}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"uacute", 0xFA}, {"ugrave", 0xF9}, {"ucirc", 0xFB}, {"yacute", 0xFD}, {"yuml", 0xFF}, {"thorn", 0xFE}, {"eth", 0xF0}};

int xml_entity(const char *s, char *out, int *out_len)
{
    if (*s != '&')
        return 0;
    const char *semi = s + 1;
    while (*semi && *semi != ';' && semi - s < 12)
        semi++;
    if (*semi != ';')
        return 0;
    unsigned cp = 0;
    if (s[1] == '#') {
        if (s[2] == 'x' || s[2] == 'X')
            cp = strtoul(s + 3, NULL, 16);
        else
            cp = strtoul(s + 2, NULL, 10);
        if (cp == 0 || cp > 0x10FFFF)
            return 0;
    }
    else {
        int len = (int)(semi - s - 1);
        for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++)
            if ((int)strlen(entities[i].name) == len && !strncmp(entities[i].name, s + 1, len)) {
                cp = entities[i].cp;
                break;
            }
        if (!cp)
            return 0;
    }
    *out_len = utf8_encode(cp, out);
    return (int)(semi - s + 1);
}

void xml_text(const char *s, const char *e, char *out, size_t n)
{
    size_t o = 0;
    bool space = false;
    while (s < e && o + 8 < n) {
        if (isspace((unsigned char)*s)) {
            space = o > 0;
            s++;
            continue;
        }
        if (space) {
            out[o++] = ' ';
            space = false;
        }
        char tmp[8];
        int tl = 0;
        int used = *s == '&' ? xml_entity(s, tmp, &tl) : 0;
        if (used) {
            memcpy(out + o, tmp, tl);
            o += tl;
            s += used;
        }
        else
            out[o++] = *s++;
    }
    if (n)
        out[o < n ? o : n - 1] = '\0';
}

void xml_inner_text(const XTag *t, char *out, size_t n)
{
    // gather text of [after, matching end) while stripping inner tags
    char *buf = malloc(n);
    if (!buf) {
        out[0] = '\0';
        return;
    }
    size_t o = 0;
    const char *p = t->after;
    int depth = 0;
    XTag tag;
    const char *te;
    while (o < n - 1) {
        const char *start = p;
        bool more = xml_next(&p, &tag, &te);
        size_t l = te - start;
        if (l > n - 1 - o)
            l = n - 1 - o;
        memcpy(buf + o, start, l);
        o += l;
        if (!more)
            break;
        if (tag.name[0] == '!')
            continue;
        if (!tag.end && !tag.self_closing && !strcmp(tag.name, t->name))
            depth++;
        if (tag.end && !strcmp(tag.name, t->name) && depth-- == 0)
            break;
        // keep a space where block tags were
        if (o < n - 1)
            buf[o++] = ' ';
    }
    buf[o] = '\0';
    xml_text(buf, buf + o, out, n);
    free(buf);
}

/* ---------- paths ---------- */

void path_join(const char *dir, const char *href, char *out, size_t n)
{
    char tmp[1024];
    if (href[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", href + 1);
    else
        snprintf(tmp, sizeof(tmp), "%s%s%s", dir, *dir ? "/" : "", href);
    char *hash = strchr(tmp, '#');
    if (hash)
        *hash = '\0';
    // %-decode
    char dec[1024];
    size_t o = 0;
    for (char *p = tmp; *p && o < sizeof(dec) - 1; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hx[3] = {p[1], p[2], 0};
            dec[o++] = (char)strtol(hx, NULL, 16);
            p += 2;
        }
        else
            dec[o++] = *p;
    }
    dec[o] = '\0';
    // resolve . and ..
    char *parts[64];
    int np = 0;
    for (char *tok = strtok(dec, "/"); tok && np < 64; tok = strtok(NULL, "/")) {
        if (!strcmp(tok, "."))
            continue;
        if (!strcmp(tok, "..")) {
            if (np)
                np--;
            continue;
        }
        parts[np++] = tok;
    }
    o = 0;
    out[0] = '\0';
    for (int i = 0; i < np; i++)
        o += snprintf(out + o, o < n ? n - o : 0, "%s%s", i ? "/" : "", parts[i]);
}

/* ---------- document building ---------- */

typedef struct {
    Doc *d;
    int bold, italic, heading, center, indent, pre;
    bool open, space, next_cont;
    bool bullet;
} Builder;

static void *grow(void *p, int *cap, int need, size_t elem)
{
    if (need <= *cap)
        return p;
    int c = *cap ? *cap : 64;
    while (c < need)
        c *= 2;
    void *np = realloc(p, c * elem);
    if (!np)
        return p;
    *cap = c;
    return np;
}

static void put_bytes(Doc *d, const char *s, int n)
{
    d->text = grow(d->text, &d->cap, d->len + n + 1, 1);
    memcpy(d->text + d->len, s, n);
    d->len += n;
    d->text[d->len] = '\0';
}

static Block *new_block(Doc *d, int type)
{
    d->blocks = grow(d->blocks, &d->cap_blocks, d->nblocks + 1, sizeof(Block));
    Block *b = &d->blocks[d->nblocks++];
    memset(b, 0, sizeof(*b));
    b->type = type;
    b->run_first = d->nruns;
    b->off = d->len;
    return b;
}

static void end_block(Builder *B)
{
    Doc *d = B->d;
    if (!B->open)
        return;
    B->open = false;
    B->space = false;
    Block *b = &d->blocks[d->nblocks - 1];
    if (b->run_count == 0) {
        d->nblocks--;
        return;
    }
    if (b->heading && !d->title[0]) {
        int o = 0;
        for (int r = b->run_first; r < b->run_first + b->run_count; r++) {
            int l = d->runs[r].len;
            if (o + l >= (int)sizeof(d->title) - 1)
                l = sizeof(d->title) - 1 - o;
            memcpy(d->title + o, d->text + d->runs[r].start, l);
            o += l;
        }
        d->title[o] = '\0';
    }
}

static void put_char(Builder *B, const char *s, int n)
{
    Doc *d = B->d;
    unsigned char style = (B->bold ? ST_BOLD : 0) | (B->italic ? ST_ITALIC : 0);
    if (!B->open) {
        Block *b = new_block(d, B_TEXT);
        b->heading = B->heading;
        b->center = B->center > 0;
        b->indent = B->indent > 3 ? 3 : B->indent;
        b->cont = B->next_cont;
        B->next_cont = false;
        B->open = true;
        B->space = false;
        if (B->bullet) {
            B->bullet = false;
            put_char(B, "\xE2\x80\xA2 ", 4);
        }
    }
    Block *b = &d->blocks[d->nblocks - 1];
    if (B->space && b->run_count) {
        B->space = false;
        put_char(B, " ", 1);
    }
    B->space = false;
    Run *last = b->run_count ? &d->runs[d->nruns - 1] : NULL;
    if (last && last->style == style && last->start + last->len == d->len)
        last->len += n;
    else {
        d->runs = grow(d->runs, &d->cap_runs, d->nruns + 1, sizeof(Run));
        Run *r = &d->runs[d->nruns++];
        r->start = d->len;
        r->len = n;
        r->style = style;
        b->run_count++;
    }
    put_bytes(d, s, n);
}

static void add_text(Builder *B, const char *s, const char *e)
{
    while (s < e) {
        unsigned char c = *s;
        if (c == '\r') {
            s++;
            continue;
        }
        if (B->pre && c == '\n') {
            end_block(B);
            B->next_cont = true;
            s++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (B->open)
                B->space = true;
            s++;
            continue;
        }
        if (c == '&') {
            char tmp[8];
            int tl = 0, used = xml_entity(s, tmp, &tl);
            if (used) {
                put_char(B, tmp, tl);
                s += used;
                continue;
            }
        }
        int n = 1;
        if (c >= 0xC0)
            while (s + n < e && ((unsigned char)s[n] & 0xC0) == 0x80 && n < 4)
                n++;
        put_char(B, s, n);
        s += n;
    }
}

enum { F_BOLD = 1,
       F_ITALIC = 2,
       F_INDENT = 4,
       F_CENTER = 8,
       F_PRE = 16,
       F_BLOCK = 32 };

typedef struct {
    char name[32];
    int flags;
    int heading;
} OpenTag;

static bool is_block_tag(const char *n)
{
    static const char *tags[] = {"p", "div", "li", "dt", "dd", "blockquote", "section", "article", "header", "footer", "aside", "nav", "figure", "figcaption", "ul", "ol", "dl", "table", "tr", "center", "body", "pre", "address", "hgroup", "caption", "main", "h1", "h2", "h3", "h4", "h5", "h6"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (!strcmp(n, tags[i]))
            return true;
    return false;
}

static bool wants_center(const XTag *t)
{
    char v[256];
    if (xml_attr(t, "style", v, sizeof(v)) && strstr(v, "center"))
        return true;
    if (xml_attr(t, "class", v, sizeof(v))) {
        for (char *p = v; *p; p++)
            *p = tolower((unsigned char)*p);
        if (strstr(v, "center") || strstr(v, "centre"))
            return true;
    }
    return false;
}

static void apply(Builder *B, int flags, int heading, int sign)
{
    if (flags & F_BOLD)
        B->bold += sign;
    if (flags & F_ITALIC)
        B->italic += sign;
    if (flags & F_INDENT)
        B->indent += sign;
    if (flags & F_CENTER)
        B->center += sign;
    if (flags & F_PRE)
        B->pre += sign;
    if (heading)
        B->heading = sign > 0 ? heading : 0;
}

Doc *doc_from_html(const char *html, const char *base_dir)
{
    Doc *d = calloc(1, sizeof(Doc));
    Builder B = {.d = d};
    OpenTag stack[64];
    int sp = 0;
    const char *p = html, *te;
    XTag t;
    while (1) {
        const char *start = p;
        bool more = xml_next(&p, &t, &te);
        add_text(&B, start, te);
        if (!more)
            break;
        const char *n = t.name;
        if (n[0] == '!')
            continue;
        if (!t.end && !t.self_closing && (!strcmp(n, "head") || !strcmp(n, "style") || !strcmp(n, "script") || !strcmp(n, "title"))) {
            // skip everything up to the end tag
            char endtag[40];
            snprintf(endtag, sizeof(endtag), "</%s", n);
            const char *e = strcasestr(p, endtag);
            if (!e) {
                // namespaced end tags are rare enough; just stop scanning this element
                break;
            }
            p = e;
            continue;
        }
        if (!strcmp(n, "br")) {
            end_block(&B);
            B.next_cont = true;
            continue;
        }
        if (!strcmp(n, "hr") && !t.end) {
            end_block(&B);
            new_block(d, B_RULE);
            put_bytes(d, "\n", 1);
            continue;
        }
        if ((!strcmp(n, "img") || !strcmp(n, "image")) && !t.end) {
            char src[512];
            if (xml_attr(&t, "src", src, sizeof(src)) || xml_attr(&t, "href", src, sizeof(src))) {
                end_block(&B);
                Block *b = new_block(d, B_IMAGE);
                char full[1024];
                path_join(base_dir, src, full, sizeof(full));
                b->src = strdup(full);
                put_bytes(d, "\n", 1);
            }
            continue;
        }
        if (!strcmp(n, "td") || !strcmp(n, "th")) {
            if (B.open)
                B.space = true;
            continue;
        }
        int flags = 0, heading = 0;
        if (is_block_tag(n))
            flags |= F_BLOCK;
        if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && !n[2]) {
            heading = n[1] - '0';
            flags |= F_BOLD;
        }
        if (!strcmp(n, "b") || !strcmp(n, "strong"))
            flags |= F_BOLD;
        if (!strcmp(n, "i") || !strcmp(n, "em") || !strcmp(n, "cite") || !strcmp(n, "var") || !strcmp(n, "dfn"))
            flags |= F_ITALIC;
        if (!strcmp(n, "blockquote") || !strcmp(n, "li") || !strcmp(n, "dd"))
            flags |= F_INDENT;
        if (!strcmp(n, "pre"))
            flags |= F_PRE;
        if (!flags)
            continue;
        if (t.end) {
            // pop to the matching tag (tolerates unclosed tags)
            int i = sp - 1;
            while (i >= 0 && strcmp(stack[i].name, n))
                i--;
            if (i < 0)
                continue;
            while (sp > i) {
                OpenTag *o = &stack[--sp];
                if (o->flags & F_BLOCK)
                    end_block(&B);
                apply(&B, o->flags, o->heading, -1);
            }
            if (flags & F_BLOCK)
                end_block(&B);
            continue;
        }
        if (t.self_closing)
            continue;
        if (flags & F_BLOCK) {
            end_block(&B);
            if (heading || wants_center(&t) || !strcmp(n, "center"))
                flags |= F_CENTER;
        }
        if (!strcmp(n, "li"))
            B.bullet = true;
        if (sp < 64) {
            snprintf(stack[sp].name, sizeof(stack[sp].name), "%s", n);
            stack[sp].flags = flags;
            stack[sp].heading = heading;
            sp++;
            apply(&B, flags, heading, 1);
        }
    }
    end_block(&B);
    return d;
}

Doc *doc_from_text(const char *txt, long len)
{
    Doc *d = calloc(1, sizeof(Doc));
    Builder B = {.d = d};
    // paragraphs are separated by blank lines when the text has them, else every line is one
    long lines = 0, blanks = 0;
    const char *s = txt, *e = txt + len;
    bool line_empty = true;
    for (const char *p = s; p < e; p++) {
        if (*p == '\n') {
            lines++;
            if (line_empty)
                blanks++;
            line_empty = true;
        }
        else if (!isspace((unsigned char)*p))
            line_empty = false;
    }
    bool blank_sep = blanks * 8 > lines;
    const char *p = s;
    while (p < e) {
        const char *nl = memchr(p, '\n', e - p);
        const char *le = nl ? nl : e;
        const char *a = p, *b = le;
        while (a < b && isspace((unsigned char)*a))
            a++;
        while (b > a && isspace((unsigned char)b[-1]))
            b--;
        if (a == b) {
            end_block(&B);
        }
        else if (txt_heading_line(a, (int)(b - a))) {
            end_block(&B);
            B.heading = 2;
            B.bold = 1;
            B.center = 1;
            for (const char *q = a; q < b;) {
                int n = 1;
                if ((unsigned char)*q >= 0xC0)
                    while (q + n < b && ((unsigned char)q[n] & 0xC0) == 0x80 && n < 4)
                        n++;
                if (*q == ' ' || *q == '\t')
                    B.space = true;
                else
                    put_char(&B, q, n);
                q += n;
            }
            end_block(&B);
            B.heading = 0;
            B.bold = 0;
            B.center = 0;
        }
        else {
            if (!blank_sep)
                end_block(&B);
            else if (B.open)
                B.space = true;
            for (const char *q = a; q < b;) {
                int n = 1;
                if ((unsigned char)*q >= 0xC0)
                    while (q + n < b && ((unsigned char)q[n] & 0xC0) == 0x80 && n < 4)
                        n++;
                if (*q == ' ' || *q == '\t' || *q == '\r')
                    B.space = B.open;
                else
                    put_char(&B, q, n);
                q += n;
            }
        }
        p = nl ? nl + 1 : e;
    }
    end_block(&B);
    return d;
}

bool txt_heading_line(const char *s, int n)
{
    if (n <= 0 || n > 60)
        return false;
    static const char *words[] = {"kapittel ", "chapter ", "kapitel ", "del ", "part ", "prolog", "epilog", "prologue", "epilogue", "bok ", "book "};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        int l = strlen(words[i]);
        if (n >= l && !strncasecmp(s, words[i], l)) {
            // "Del 3", "Chapter IV", "Kapittel 1: Navn", but not ordinary sentences
            if (words[i][l - 1] != ' ')
                return n <= l + 2;
            // a number or roman numeral must follow, then the end or punctuation
            const char *q = s + l, *e = s + n;
            const char *num = q;
            while (q < e && (isdigit((unsigned char)*q) || strchr("IVXLC", *q)))
                q++;
            return q > num && (q == e || strchr(":.- ", *q));
        }
    }
    return false;
}

void doc_free(Doc *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->nblocks; i++)
        free(d->blocks[i].src);
    free(d->blocks);
    free(d->runs);
    free(d->text);
    free(d);
}

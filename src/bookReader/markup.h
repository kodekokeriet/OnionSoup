// Tolerant XML/XHTML scanning and conversion of book text into a flat document
#ifndef BOOK_MARKUP_H__
#define BOOK_MARKUP_H__

#include <stdbool.h>
#include <stddef.h>

/* ---------- tag scanner ---------- */

typedef struct {
    char name[32];     // local name, lower case ("dc:title" -> "title")
    bool end;          // </name>
    bool self_closing; // <name/>
    const char *attrs; // raw attribute text
    int attrs_len;
    const char *after; // first byte after '>'
} XTag;

// Find the next tag at or after *p. Text before it is [*p, tag start). Returns false at the end.
bool xml_next(const char **p, XTag *t, const char **text_end);
// Copy an attribute value (local name match, entities decoded). Returns false if missing.
bool xml_attr(const XTag *t, const char *name, char *out, size_t n);
// Decode entities and collapse whitespace of [s, e) into out
void xml_text(const char *s, const char *e, char *out, size_t n);
// Read the text content up to the matching end tag of t (inner tags stripped)
void xml_inner_text(const XTag *t, char *out, size_t n);
// Decode one entity starting at '&'. Writes UTF-8 to out (>= 8 bytes), returns bytes consumed or 0.
int xml_entity(const char *s, char *out, int *out_len);
int utf8_encode(unsigned cp, char *out);

/* ---------- flat document ---------- */

enum { ST_BOLD = 1,
       ST_ITALIC = 2 };
enum { B_TEXT,
       B_IMAGE,
       B_RULE };

typedef struct {
    int start, len;
    unsigned char style;
} Run;

typedef struct {
    unsigned char type;
    unsigned char heading; // 0 = body text, 1..6 = <h1>..<h6>
    unsigned char center;
    unsigned char indent; // list / blockquote depth
    unsigned char cont;   // continues the previous block (after <br>), no paragraph gap
    int run_first, run_count;
    int off;   // offset in Doc.text where the block starts
    char *src; // B_IMAGE: path inside the book
} Block;

typedef struct {
    char *text;
    int len, cap;
    Run *runs;
    int nruns, cap_runs;
    Block *blocks;
    int nblocks, cap_blocks;
    char title[160]; // first heading, if any
} Doc;

// base_dir: directory of the chapter inside the zip, used to resolve image paths ("" for none)
Doc *doc_from_html(const char *html, const char *base_dir);
Doc *doc_from_text(const char *txt, long len);
void doc_free(Doc *d);
// "Kapittel 3", "Chapter IV", "Prolog" ... used to split and style plain text books
bool txt_heading_line(const char *s, int n);

// Resolve a relative href ("../img/a.jpg#x") against a directory, also %-decoding it
void path_join(const char *dir, const char *href, char *out, size_t n);

#endif // BOOK_MARKUP_H__

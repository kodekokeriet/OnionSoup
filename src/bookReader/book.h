// Books on the SD card: EPUB and plain text
#ifndef BOOK_BOOK_H__
#define BOOK_BOOK_H__

#include "markup.h"
#include "zip.h"

typedef enum { FMT_EPUB,
               FMT_TXT } BookFormat;

typedef struct {
    char title[160];
    int chapter; // index into Book.chapters
} TocEntry;

typedef struct {
    char path[512];  // inside the zip (EPUB); unused for TXT
    long start, end; // byte range in Book.txt (TXT)
    long weight;     // size, used for the overall progress
} Chapter;

typedef struct {
    char file[512];
    BookFormat fmt;
    char title[256], author[256];
    char *description;
    char cover[512]; // EPUB: cover image path inside the zip

    Zip *zip;
    char *txt; // TXT: whole file, as UTF-8
    long txt_len;

    Chapter *chapters;
    int nchapters;
    long total_weight;
    TocEntry *toc;
    int ntoc;
} Book;

// Is this a file we can read (by extension)?
bool book_supported(const char *name);
// Title and author guessed from a file name such as "Forfatter - Tittel.epub"
void book_guess_from_name(const char *file, char *title, size_t tn, char *author, size_t an);

// meta_only: only title, author, description and cover (fast, used while scanning the shelf)
Book *book_open(const char *file, bool meta_only);
void book_close(Book *b);
Doc *book_chapter(Book *b, int index);
// Raw image bytes (cover or an illustration path inside the book)
unsigned char *book_image(Book *b, const char *path, long *len);
// Text title of a chapter: from the table of contents, else "Kapittel N"
void book_chapter_title(Book *b, int index, char *out, size_t n);

#endif // BOOK_BOOK_H__

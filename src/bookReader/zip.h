// Minimal read-only ZIP reader (stored + deflate), enough for EPUB files.
#ifndef BOOK_ZIP_H__
#define BOOK_ZIP_H__

#include <stdio.h>

typedef struct {
    char *name;
    int method;
    long comp_size, size, offset;
} ZipEntry;

typedef struct {
    FILE *fp;
    ZipEntry *entries;
    int count;
} Zip;

Zip *zip_open(const char *path);
void zip_close(Zip *z);
// Find an entry by exact name, falling back to a case-insensitive match
ZipEntry *zip_find(Zip *z, const char *name);
// Read a whole entry into a NUL-terminated buffer; returns NULL on error
char *zip_read(Zip *z, ZipEntry *e, long *out_len);
char *zip_read_name(Zip *z, const char *name, long *out_len);

#endif // BOOK_ZIP_H__

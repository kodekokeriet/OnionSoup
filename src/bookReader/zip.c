#include "zip.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "stb/stb_image.h"

static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

Zip *zip_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    // the end of central directory record is in the last 64 KB + 22 bytes
    long tail = fsize < 65557 ? fsize : 65557;
    unsigned char *buf = malloc(tail);
    if (!buf || fseek(fp, fsize - tail, SEEK_SET) != 0 || fread(buf, 1, tail, fp) != (size_t)tail) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    int count = rd16(buf + eocd + 10);
    long cd_size = rd32(buf + eocd + 12);
    long cd_off = rd32(buf + eocd + 16);
    free(buf);
    if (cd_off + cd_size > fsize || count <= 0) {
        fclose(fp);
        return NULL;
    }
    unsigned char *cd = malloc(cd_size);
    if (!cd || fseek(fp, cd_off, SEEK_SET) != 0 || fread(cd, 1, cd_size, fp) != (size_t)cd_size) {
        free(cd);
        fclose(fp);
        return NULL;
    }
    Zip *z = calloc(1, sizeof(Zip));
    z->fp = fp;
    z->entries = calloc(count, sizeof(ZipEntry));
    long p = 0;
    for (int i = 0; i < count && p + 46 <= cd_size; i++) {
        unsigned char *h = cd + p;
        if (rd32(h) != 0x02014b50)
            break;
        int nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (p + 46 + nlen > cd_size)
            break;
        ZipEntry *e = &z->entries[z->count++];
        e->method = rd16(h + 10);
        e->comp_size = rd32(h + 20);
        e->size = rd32(h + 24);
        e->offset = rd32(h + 42);
        e->name = malloc(nlen + 1);
        memcpy(e->name, h + 46, nlen);
        e->name[nlen] = '\0';
        p += 46 + nlen + xlen + clen;
    }
    free(cd);
    return z;
}

void zip_close(Zip *z)
{
    if (!z)
        return;
    for (int i = 0; i < z->count; i++)
        free(z->entries[i].name);
    free(z->entries);
    fclose(z->fp);
    free(z);
}

ZipEntry *zip_find(Zip *z, const char *name)
{
    if (!z || !name)
        return NULL;
    while (*name == '/')
        name++;
    for (int i = 0; i < z->count; i++)
        if (!strcmp(z->entries[i].name, name))
            return &z->entries[i];
    for (int i = 0; i < z->count; i++)
        if (!strcasecmp(z->entries[i].name, name))
            return &z->entries[i];
    return NULL;
}

char *zip_read(Zip *z, ZipEntry *e, long *out_len)
{
    if (!z || !e)
        return NULL;
    unsigned char lh[30];
    if (fseek(z->fp, e->offset, SEEK_SET) != 0 || fread(lh, 1, 30, z->fp) != 30 || rd32(lh) != 0x04034b50)
        return NULL;
    long data = e->offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (e->comp_size > 64 * 1024 * 1024 || e->size > 64 * 1024 * 1024)
        return NULL;
    char *comp = malloc(e->comp_size + 1);
    if (!comp)
        return NULL;
    if (fseek(z->fp, data, SEEK_SET) != 0 || fread(comp, 1, e->comp_size, z->fp) != (size_t)e->comp_size) {
        free(comp);
        return NULL;
    }
    char *out = NULL;
    long len = 0;
    if (e->method == 0) {
        out = comp;
        len = e->comp_size;
    }
    else if (e->method == 8) {
        int got = 0;
        out = stbi_zlib_decode_malloc_guesssize_headerflag(comp, (int)e->comp_size, (int)e->size + 1, &got, 0);
        free(comp);
        if (!out)
            return NULL;
        len = got;
        // make room for the terminating NUL
        char *grown = realloc(out, len + 1);
        if (!grown) {
            free(out);
            return NULL;
        }
        out = grown;
    }
    else {
        free(comp);
        return NULL;
    }
    out[len] = '\0';
    if (out_len)
        *out_len = len;
    return out;
}

char *zip_read_name(Zip *z, const char *name, long *out_len)
{
    return zip_read(z, zip_find(z, name), out_len);
}

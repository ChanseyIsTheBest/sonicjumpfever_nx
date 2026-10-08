/* sj_apkzip.c -- minimal zip reader for the user's APK.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * Only what an APK needs: no ZIP64 (APKs are far below 4 GB), no encryption,
 * no multi-disk. Methods 0 (stored) and 8 (deflate) are supported, which is
 * everything aapt emits.
 *
 * The central directory is cached after the first lookup -- music changes
 * re-query it on every track and the APK has well over a thousand entries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <switch.h>

#include "sj_apkzip.h"

#define EOCD_SIG  0x06054b50u
#define CDIR_SIG  0x02014b50u
#define LOCAL_SIG 0x04034b50u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- central directory cache --------------------------------------------- */

static Mutex    g_lock;
static char     g_cached_path[600];
static uint8_t *g_cdir;
static size_t   g_cdir_size;
static uint32_t g_cdir_count;

/* Load the central directory of `path` into the cache. Caller holds g_lock. */
static int load_cdir(const char *path)
{
    FILE *f;
    long fsize, tail_start, i;
    size_t tail_len;
    uint8_t *tail;
    uint32_t cd_off = 0, cd_size = 0;
    uint16_t cd_count = 0;
    int found = 0;

    if (g_cdir && !strcmp(g_cached_path, path)) return 0;

    free(g_cdir);
    g_cdir = NULL;
    g_cdir_size = 0;
    g_cdir_count = 0;
    g_cached_path[0] = '\0';

    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 22) { fclose(f); return -1; }

    /* The end-of-central-directory record is the last 22 bytes plus an
     * optional comment of up to 64 KB, so scan backwards over that window. */
    tail_start = fsize - (22 + 65535);
    if (tail_start < 0) tail_start = 0;
    tail_len = (size_t)(fsize - tail_start);
    tail = malloc(tail_len);
    if (!tail) { fclose(f); return -1; }
    if (fseek(f, tail_start, SEEK_SET) != 0 ||
        fread(tail, 1, tail_len, f) != tail_len) {
        free(tail); fclose(f); return -1;
    }
    for (i = (long)tail_len - 22; i >= 0; i--) {
        if (rd32(tail + i) == EOCD_SIG) {
            cd_count = rd16(tail + i + 10);
            cd_size  = rd32(tail + i + 12);
            cd_off   = rd32(tail + i + 16);
            found = 1;
            break;
        }
    }
    free(tail);
    if (!found || (long)cd_off + (long)cd_size > fsize) { fclose(f); return -1; }

    g_cdir = malloc(cd_size ? cd_size : 1);
    if (!g_cdir) { fclose(f); return -1; }
    if (fseek(f, (long)cd_off, SEEK_SET) != 0 ||
        fread(g_cdir, 1, cd_size, f) != cd_size) {
        free(g_cdir); g_cdir = NULL; fclose(f); return -1;
    }
    fclose(f);
    g_cdir_size  = cd_size;
    g_cdir_count = cd_count;
    snprintf(g_cached_path, sizeof(g_cached_path), "%s", path);
    return 0;
}

int sj_zip_find(const char *zip_path, const char *name, SjZipEntry *out)
{
    static int lock_ready;
    size_t pos = 0, name_len;
    uint32_t n;
    uint32_t lho = 0;
    int hit = 0;
    FILE *f;
    uint8_t local[30];

    if (!zip_path || !*zip_path || !name || !out) return -1;
    if (!lock_ready) { mutexInit(&g_lock); lock_ready = 1; }
    name_len = strlen(name);

    mutexLock(&g_lock);
    if (load_cdir(zip_path) != 0) { mutexUnlock(&g_lock); return -1; }

    for (n = 0; n < g_cdir_count && pos + 46 <= g_cdir_size; n++) {
        const uint8_t *e = g_cdir + pos;
        uint16_t nlen, elen, clen;
        if (rd32(e) != CDIR_SIG) break;
        nlen = rd16(e + 28);
        elen = rd16(e + 30);
        clen = rd16(e + 32);
        if (pos + 46 + nlen > g_cdir_size) break;
        if (nlen == name_len && !memcmp(e + 46, name, name_len)) {
            out->method    = rd16(e + 10);
            out->crc32     = rd32(e + 16);
            out->comp_size = rd32(e + 20);
            out->size      = rd32(e + 24);
            lho            = rd32(e + 42);
            hit = 1;
            break;
        }
        pos += 46 + (size_t)nlen + elen + clen;
    }
    mutexUnlock(&g_lock);
    if (!hit) return -1;

    /* The data starts after the LOCAL header, whose name/extra lengths can
     * differ from the central copy (aapt pads extra fields for alignment). */
    f = fopen(zip_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)lho, SEEK_SET) != 0 || fread(local, 1, 30, f) != 30 ||
        rd32(local) != LOCAL_SIG) {
        fclose(f);
        return -1;
    }
    fclose(f);
    out->data_offset = (uint64_t)lho + 30 + rd16(local + 26) + rd16(local + 28);
    return 0;
}

/* Stream an entry's uncompressed bytes to `sink`. Returns 0 on success. */
typedef int (*sink_fn)(void *ctx, const void *buf, size_t len);

static int stream_entry(const char *zip_path, const SjZipEntry *e,
                        sink_fn sink, void *ctx)
{
    enum { CHUNK = 64 * 1024 };
    FILE *f;
    uint8_t *in = NULL, *outb = NULL;
    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint32_t left = e->comp_size;
    uint64_t produced = 0;
    int rc = -1;

    f = fopen(zip_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)e->data_offset, SEEK_SET) != 0) goto done;
    in = malloc(CHUNK);
    outb = malloc(CHUNK);
    if (!in || !outb) goto done;

    if (e->method == SJ_ZIP_STORED) {
        while (left) {
            size_t want = left > CHUNK ? CHUNK : left;
            if (fread(in, 1, want, f) != want) goto done;
            crc = crc32(crc, in, (uInt)want);
            if (sink(ctx, in, want) != 0) goto done;
            left -= (uint32_t)want;
            produced += want;
        }
    } else if (e->method == SJ_ZIP_DEFLATED) {
        z_stream zs;
        int zr = Z_OK;
        memset(&zs, 0, sizeof(zs));
        /* Negative window bits: raw deflate, no zlib header -- zip entries
         * are bare deflate streams. */
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) goto done;
        for (;;) {
            size_t got;
            /* Feed only when inflate has consumed everything it was given.
             * Running out of input is NOT the end: the last block can still
             * have output pending, which only comes out on further calls with
             * fresh output space. Stopping as soon as the input was used up
             * lost the tail of the library. */
            if (zs.avail_in == 0 && left > 0) {
                size_t want = left > CHUNK ? CHUNK : left;
                if (fread(in, 1, want, f) != want) { zr = Z_DATA_ERROR; break; }
                left -= (uint32_t)want;
                zs.next_in = in;
                zs.avail_in = (uInt)want;
            }
            zs.next_out = outb;
            zs.avail_out = CHUNK;
            zr = inflate(&zs, Z_NO_FLUSH);
            if (zr != Z_OK && zr != Z_STREAM_END) break;   /* incl. Z_BUF_ERROR */
            got = CHUNK - zs.avail_out;
            if (got) {
                crc = crc32(crc, outb, (uInt)got);
                if (sink(ctx, outb, got) != 0) { zr = Z_DATA_ERROR; break; }
                produced += got;
            }
            if (zr == Z_STREAM_END) break;
            /* No output, no input left, stream not finished: truncated. */
            if (!got && zs.avail_in == 0 && left == 0) { zr = Z_DATA_ERROR; break; }
        }
        inflateEnd(&zs);
        if (zr != Z_STREAM_END) goto done;
    } else {
        printf("sj_zip: unsupported compression method %u\n", e->method);
        goto done;
    }

    if (produced != e->size) {
        printf("sj_zip: size mismatch (%llu, expected %u)\n",
               (unsigned long long)produced, e->size);
        goto done;
    }
    if (crc != e->crc32) {
        printf("sj_zip: CRC mismatch (%08x, expected %08x)\n", crc, e->crc32);
        goto done;
    }
    rc = 0;
done:
    free(in);
    free(outb);
    fclose(f);
    return rc;
}

static int file_sink(void *ctx, const void *buf, size_t len)
{
    return fwrite(buf, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

int sj_zip_extract(const char *zip_path, const char *name, const char *dest_path)
{
    SjZipEntry e;
    char tmp[620];
    FILE *out;
    int rc;

    if (sj_zip_find(zip_path, name, &e) != 0) return -1;

    snprintf(tmp, sizeof(tmp), "%s.part", dest_path);
    out = fopen(tmp, "wb");
    if (!out) return -1;
    rc = stream_entry(zip_path, &e, file_sink, out);
    if (fclose(out) != 0) rc = -1;
    if (rc != 0) { remove(tmp); return -1; }

    remove(dest_path);                     /* rename will not overwrite */
    if (rename(tmp, dest_path) != 0) { remove(tmp); return -1; }
    return 0;
}

typedef struct { uint8_t *p; size_t len, cap; } MemSink;

static int mem_sink(void *ctx, const void *buf, size_t len)
{
    MemSink *m = ctx;
    if (m->len + len > m->cap) return -1;
    memcpy(m->p + m->len, buf, len);
    m->len += len;
    return 0;
}

void *sj_zip_read_alloc(const char *zip_path, const SjZipEntry *e, size_t *out_size)
{
    MemSink m;
    if (!e) return NULL;
    m.p = malloc(e->size ? e->size : 1);
    m.len = 0;
    m.cap = e->size;
    if (!m.p) return NULL;
    if (stream_entry(zip_path, e, mem_sink, &m) != 0) { free(m.p); return NULL; }
    if (out_size) *out_size = m.len;
    return m.p;
}

/* sj_apkzip.h -- just enough of a zip reader to use the APK directly.
 * MIT licensed, see LICENSE.
 *
 * Two jobs:
 *   - extract lib/arm64-v8a/libsonicjumpfever.so on first boot, so the user
 *     only has to copy their APK next to the .nro;
 *   - locate assets/<track>.m4a inside the APK, so the music streams straight
 *     out of it (the .m4a entries are STORED, i.e. uncompressed, in this APK).
 *
 * The engine itself never uses this: it opens the APK with its own
 * sl::ZipFile reader, exactly as on Android.
 */
#ifndef SJ_APKZIP_H
#define SJ_APKZIP_H

#include <stdint.h>
#include <stddef.h>

#define SJ_ZIP_STORED   0
#define SJ_ZIP_DEFLATED 8

typedef struct {
    uint64_t data_offset;   /* absolute offset of the entry's data in the zip */
    uint32_t comp_size;
    uint32_t size;          /* uncompressed */
    uint32_t crc32;
    uint16_t method;        /* SJ_ZIP_STORED / SJ_ZIP_DEFLATED */
} SjZipEntry;

/* Look up `name` (exact, case-sensitive, e.g. "assets/frontend.m4a").
 * Returns 0 and fills `out` on success, -1 if absent or the zip is unreadable. */
int sj_zip_find(const char *zip_path, const char *name, SjZipEntry *out);

/* Extract one entry to `dest_path`, verifying its CRC. Writes to a temporary
 * file first and renames it into place, so an interrupted extraction never
 * leaves a truncated library behind. Returns 0 on success. */
int sj_zip_extract(const char *zip_path, const char *name, const char *dest_path);

/* Read one entry fully into a malloc'd buffer (inflating if needed).
 * Returns NULL on failure; *out_size receives the size. Caller frees. */
void *sj_zip_read_alloc(const char *zip_path, const SjZipEntry *e, size_t *out_size);

#endif

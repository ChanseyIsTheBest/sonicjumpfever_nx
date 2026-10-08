/* sj_decode.h -- on-device streaming audio decode. MIT, see LICENSE.
 *
 * Backed by ffmpeg (switch-ffmpeg from devkitPro portlibs), so .m4a (AAC in
 * MP4) plays straight from the APK's assets with no PC-side conversion. .ogg,
 * .mp3 and .wav work through the same path.
 *
 * Output is always interleaved signed 16-bit STEREO at the requested rate.
 */
#ifndef SJ_DECODE_H
#define SJ_DECODE_H

#include <stdint.h>
#include <stddef.h>

typedef struct SjDecoder SjDecoder;

typedef struct {
    int         src_rate;      /* the file's own sample rate  */
    int         src_channels;  /* the file's own channel count */
    int         duration_ms;   /* 0 if the container does not say */
    const char *codec_name;    /* "aac", "vorbis", ... (owned by ffmpeg) */
} SjDecodeInfo;

/* Open `path`, converting to stereo S16 at `out_rate`. NULL on failure.
 * `info` may be NULL. */
SjDecoder *sj_decoder_open(const char *path, int out_rate, SjDecodeInfo *info);

/* Open the byte range [offset, offset + size) of `path` as if it were a file
 * of its own -- used to play a STORED .m4a straight out of the APK. A size of
 * -1 means "to the end of the file". */
SjDecoder *sj_decoder_open_range(const char *path, int64_t offset, int64_t size,
                                 int out_rate, SjDecodeInfo *info);

/* Decode from a malloc'd buffer. Ownership of `data` passes to the decoder in
 * every case, including failure. */
SjDecoder *sj_decoder_open_mem(void *data, size_t size, int out_rate,
                               SjDecodeInfo *info);

/* Fill `dst` with up to `frames` stereo frames. Returns frames written; a
 * short count means end of stream. */
int sj_decoder_read(SjDecoder *d, int16_t *dst, int frames);

/* Seek back to the start for looping. Returns 1 on success. */
int sj_decoder_rewind(SjDecoder *d);

void sj_decoder_close(SjDecoder *d);

#endif

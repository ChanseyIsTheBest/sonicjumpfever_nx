/* sj_decode.c -- streaming audio decode on the Switch, via ffmpeg.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * WHY FFMPEG
 * ----------
 * Sonic Jump Fever's soundtrack is 12 .m4a files: AAC in an MP4 container.
 * The Switch has no AAC decoder of its own, but devkitPro ships switch-ffmpeg
 * as a portlib, so we decode on-device. The tracks are read straight out of
 * the user's APK -- nothing is converted or even extracted.
 *
 * Using libavformat rather than a bare AAC decoder matters: .m4a is AAC inside
 * MP4, so something has to parse the container, find the audio stream, and
 * hand up whole frames. Doing that by hand is the bulk of the work and all of
 * the bugs.
 *
 * The same path handles .ogg, .mp3 and .wav, so a user who already converted
 * their music (or who prefers the smaller files) is not penalised.
 *
 * OUTPUT CONTRACT
 * ---------------
 * Always interleaved signed 16-bit stereo at the rate the caller asks for.
 * libswresample does the rate, layout and format conversion, so the mixer
 * never has to think about what the source actually was.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sj_decode.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define OUT_CHANNELS 2

/* --- custom I/O ------------------------------------------------------------
 * avformat_open_input() parses its filename as a URL, so "sdmc:/switch/..."
 * is read as protocol "sdmc" -- which does not exist -- and the open fails
 * before it ever touches the file. That is not a corner case here: sdmc: is
 * the normal device prefix on Switch, and it is exactly what sj_home() hands
 * out.
 *
 * Rather than mangle paths to suit ffmpeg's parser, hand it an AVIOContext
 * backed by plain fopen(). ffmpeg then never sees the path at all, and the
 * music path reads files the same way as the rest of the port.
 * ------------------------------------------------------------------------ */
#define SJ_IO_BUFFER 32768

/* The I/O source is a window onto a file: [base, base + size). For a loose
 * .m4a that is the whole file; for a track read straight out of the APK it is
 * the entry's bytes inside the zip (the .m4a entries are STORED, so they are
 * a plain contiguous run). ffmpeg sees offsets relative to the window and has
 * no idea it is inside an archive. A deflated entry would be inflated into
 * `mem` instead, with `f` left NULL. */
typedef struct {
    FILE    *f;
    uint8_t *mem;       /* owned; used when f is NULL */
    int64_t  base;
    int64_t  size;
    int64_t  pos;
} SjIoSrc;

static int sj_io_read(void *opaque, uint8_t *buf, int buf_size)
{
    SjIoSrc *s = (SjIoSrc *)opaque;
    int64_t left = s->size - s->pos;
    size_t want, n;

    if (left <= 0) return AVERROR_EOF;
    want = (size_t)(buf_size < left ? buf_size : left);
    if (s->f) {
        if (fseek(s->f, (long)(s->base + s->pos), SEEK_SET) != 0) return AVERROR(EIO);
        n = fread(buf, 1, want, s->f);
    } else {
        memcpy(buf, s->mem + s->pos, want);
        n = want;
    }
    if (n == 0) return AVERROR_EOF;
    s->pos += (int64_t)n;
    return (int)n;
}

static int64_t sj_io_seek(void *opaque, int64_t offset, int whence)
{
    SjIoSrc *s = (SjIoSrc *)opaque;
    int64_t np;

    /* MP4 needs the total size to locate its index; ffmpeg asks via
     * AVSEEK_SIZE rather than by seeking to the end itself. */
    if (whence == AVSEEK_SIZE) return s->size;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: np = offset; break;
    case SEEK_CUR: np = s->pos + offset; break;
    case SEEK_END: np = s->size + offset; break;
    default: return -1;
    }
    if (np < 0 || np > s->size) return -1;
    s->pos = np;
    return np;
}

struct SjDecoder {
    SjIoSrc          src;
    AVIOContext     *avio;
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              stream_index;
    int              out_rate;

    /* Converted samples that did not fit in the caller's buffer last time.
     * swr_convert produces whole frames' worth, which rarely lines up with the
     * mixer's block size, so the remainder has to be carried. */
    int16_t         *pending;
    int              pending_cap;    /* frames */
    int              pending_len;    /* frames */
    int              pending_pos;    /* frames already handed out */

    int              eof;
};

static void free_pending(SjDecoder *d)
{
    free(d->pending);
    d->pending = NULL;
    d->pending_cap = d->pending_len = d->pending_pos = 0;
}

void sj_decoder_close(SjDecoder *d)
{
    if (!d) return;
    if (d->swr)   swr_free(&d->swr);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->dec)   avcodec_free_context(&d->dec);
    if (d->fmt)   avformat_close_input(&d->fmt);
    /* avformat_close_input does not free a caller-supplied AVIOContext, and
     * ffmpeg may have replaced the buffer we handed it, so free what the
     * context currently points at rather than the original pointer. */
    if (d->avio) {
        av_freep(&d->avio->buffer);
        avio_context_free(&d->avio);
    }
    if (d->src.f) fclose(d->src.f);
    free(d->src.mem);
    free_pending(d);
    free(d);
}

/* Common tail of the three constructors: `d->src` is already set up. */
static SjDecoder *decoder_start(SjDecoder *d, int out_rate, SjDecodeInfo *info)
{
    const AVCodec *codec = NULL;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc;

    d->out_rate = out_rate;
    d->stream_index = -1;
    {
        unsigned char *iobuf = av_malloc(SJ_IO_BUFFER);
        if (!iobuf) { sj_decoder_close(d); return NULL; }
        d->avio = avio_alloc_context(iobuf, SJ_IO_BUFFER, 0, &d->src,
                                     sj_io_read, NULL, sj_io_seek);
        if (!d->avio) { av_free(iobuf); sj_decoder_close(d); return NULL; }
    }
    d->fmt = avformat_alloc_context();
    if (!d->fmt) { sj_decoder_close(d); return NULL; }
    d->fmt->pb    = d->avio;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* NULL filename: ffmpeg probes the container from the stream, so nothing
     * is ever parsed as a URL. */
    if (avformat_open_input(&d->fmt, NULL, NULL, NULL) < 0) {
        sj_decoder_close(d);
        return NULL;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        sj_decoder_close(d);
        return NULL;
    }

    rc = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (rc < 0 || !codec) {
        sj_decoder_close(d);
        return NULL;
    }
    d->stream_index = rc;

    d->dec = avcodec_alloc_context3(codec);
    if (!d->dec) { sj_decoder_close(d); return NULL; }
    if (avcodec_parameters_to_context(d->dec,
            d->fmt->streams[d->stream_index]->codecpar) < 0) {
        sj_decoder_close(d);
        return NULL;
    }
    /* One thread. The audio is decoded ahead of time on a worker thread
     * already (sj_music.c), and ffmpeg's own threading would add latency and
     * another set of stacks for no gain on a 23-second music loop. */
    d->dec->thread_count = 1;

    if (avcodec_open2(d->dec, codec, NULL) < 0) {
        sj_decoder_close(d);
        return NULL;
    }

    /* Resample/downmix everything to interleaved S16 stereo at out_rate. */
    rc = swr_alloc_set_opts2(&d->swr,
                             &out_layout, AV_SAMPLE_FMT_S16, out_rate,
                             &d->dec->ch_layout, d->dec->sample_fmt,
                             d->dec->sample_rate,
                             0, NULL);
    if (rc < 0 || !d->swr || swr_init(d->swr) < 0) {
        sj_decoder_close(d);
        return NULL;
    }

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) { sj_decoder_close(d); return NULL; }

    if (info) {
        info->src_rate     = d->dec->sample_rate;
        info->src_channels = d->dec->ch_layout.nb_channels;
        info->codec_name   = codec->name;
        info->duration_ms  = (d->fmt->duration > 0)
                           ? (int)(d->fmt->duration / (AV_TIME_BASE / 1000))
                           : 0;
    }
    return d;
}

SjDecoder *sj_decoder_open_range(const char *path, int64_t offset, int64_t size,
                                 int out_rate, SjDecodeInfo *info)
{
    SjDecoder *d;
    if (!path || out_rate <= 0) return NULL;
    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->src.f = fopen(path, "rb");
    if (!d->src.f) { free(d); return NULL; }
    if (size < 0) {                          /* whole file */
        if (fseek(d->src.f, 0, SEEK_END) != 0) { sj_decoder_close(d); return NULL; }
        size = ftell(d->src.f) - offset;
        if (size <= 0) { sj_decoder_close(d); return NULL; }
    }
    d->src.base = offset;
    d->src.size = size;
    return decoder_start(d, out_rate, info);
}

SjDecoder *sj_decoder_open(const char *path, int out_rate, SjDecodeInfo *info)
{
    return sj_decoder_open_range(path, 0, -1, out_rate, info);
}

SjDecoder *sj_decoder_open_mem(void *data, size_t size, int out_rate,
                               SjDecodeInfo *info)
{
    SjDecoder *d;
    if (!data || !size || out_rate <= 0) { free(data); return NULL; }
    d = calloc(1, sizeof(*d));
    if (!d) { free(data); return NULL; }
    d->src.mem  = data;                      /* ownership passes to d */
    d->src.size = (int64_t)size;
    return decoder_start(d, out_rate, info);
}

/* Grow the carry buffer to hold at least `frames`. */
static int ensure_pending(SjDecoder *d, int frames)
{
    int16_t *p;
    if (d->pending_cap >= frames) return 1;
    p = realloc(d->pending, (size_t)frames * OUT_CHANNELS * sizeof(int16_t));
    if (!p) return 0;
    d->pending = p;
    d->pending_cap = frames;
    return 1;
}

/* Move up to `want` frames out of the carry buffer. */
static int take_pending(SjDecoder *d, int16_t *dst, int want)
{
    int avail = d->pending_len - d->pending_pos;
    int n = avail < want ? avail : want;
    if (n <= 0) return 0;
    memcpy(dst, d->pending + (size_t)d->pending_pos * OUT_CHANNELS,
           (size_t)n * OUT_CHANNELS * sizeof(int16_t));
    d->pending_pos += n;
    if (d->pending_pos >= d->pending_len) d->pending_pos = d->pending_len = 0;
    return n;
}

/* Decode one packet's worth into the carry buffer. Returns 1 if it produced
 * samples, 0 at end of stream, -1 on a hard error. */
static int fill_pending(SjDecoder *d)
{
    for (;;) {
        int rc = avcodec_receive_frame(d->dec, d->frame);

        if (rc == 0) {
            int max_out = (int)swr_get_out_samples(d->swr, d->frame->nb_samples);
            uint8_t *out[1];
            int got;

            if (max_out <= 0) max_out = d->frame->nb_samples + 256;
            if (!ensure_pending(d, max_out)) return -1;

            out[0] = (uint8_t *)d->pending;
            got = swr_convert(d->swr, out, max_out,
                              (const uint8_t **)d->frame->data,
                              d->frame->nb_samples);
            av_frame_unref(d->frame);
            if (got < 0) return -1;
            if (got == 0) continue;          /* swr buffered it; keep going */
            d->pending_len = got;
            d->pending_pos = 0;
            return 1;
        }

        if (rc == AVERROR_EOF) {
            /* Flush whatever libswresample is still holding. */
            int max_out = (int)swr_get_out_samples(d->swr, 0);
            uint8_t *out[1];
            int got;
            if (max_out <= 0) return 0;
            if (!ensure_pending(d, max_out)) return -1;
            out[0] = (uint8_t *)d->pending;
            got = swr_convert(d->swr, out, max_out, NULL, 0);
            if (got <= 0) return 0;
            d->pending_len = got;
            d->pending_pos = 0;
            return 1;
        }

        if (rc != AVERROR(EAGAIN)) return -1;

        /* Decoder wants more input. */
        for (;;) {
            int r = av_read_frame(d->fmt, d->pkt);
            if (r < 0) {
                avcodec_send_packet(d->dec, NULL);   /* signal EOF, then drain */
                d->eof = 1;
                break;
            }
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;                            /* skip other streams */
            }
            r = avcodec_send_packet(d->dec, d->pkt);
            av_packet_unref(d->pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) return -1;
            break;
        }
    }
}

int sj_decoder_read(SjDecoder *d, int16_t *dst, int frames)
{
    int done = 0;
    if (!d || !dst || frames <= 0) return 0;

    while (done < frames) {
        int n = take_pending(d, dst + (size_t)done * OUT_CHANNELS, frames - done);
        if (n > 0) { done += n; continue; }
        {
            int rc = fill_pending(d);
            if (rc <= 0) break;              /* end of stream or error */
        }
    }
    return done;
}

int sj_decoder_rewind(SjDecoder *d)
{
    if (!d) return 0;
    /* Seek to the very start of the stream, not to timestamp 0 of the file:
     * some MP4s carry an edit list and the first audio sample is not at 0. */
    if (av_seek_frame(d->fmt, d->stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    avcodec_flush_buffers(d->dec);
    d->pending_len = d->pending_pos = 0;
    d->eof = 0;
    return 1;
}

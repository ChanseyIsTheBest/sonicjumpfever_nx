/* sj_music.c -- soundtrack playback behind the fake `musicplayer` class.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * The engine asks musicplayer.PlayAudioFile("frontend.m4a", loop, restart) and
 * gets exactly that track. A loose file in <gamedir>/assets/ wins if there is
 * one (so a user can swap or convert music), trying the same stem with
 * .ogg/.mp3/.wav too; otherwise it is streamed straight out of the APK.
 * sj_decode.c handles AAC-in-MP4 on-device through ffmpeg, so there is no
 * PC-side conversion or extraction step at all.
 *
 * THREE THREADS, ONE RING
 * -----------------------
 *   game thread   calls sj_music_play/stop; opens the decoder, starts a worker
 *   worker thread decodes ahead into a ring buffer, blocking when it is full
 *   audio thread  music_pull() drains the ring from inside the mixer callback
 *
 * The worker exists because AAC decode is far too expensive to do inside an
 * audio callback: a late buffer there does not just skip the music, it drops
 * out every sound effect in the game. The callback now does nothing but a
 * memcpy and a volume multiply, both bounded.
 *
 * The ring is ~680 ms at 48 kHz, which is a lot of slack for a decoder that
 * needs maybe 2% of a core -- deliberately generous, because the cost of
 * overshooting is a quarter megabyte and the cost of undershooting is audible.
 * Reading from the APK costs nothing extra: the worker seeks within the zip
 * exactly as it would within a loose file.
 *
 * SFX go through OpenSL buffer queues (opensles.c) and never reach this file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "sj_music.h"
#include "sj_decode.h"
#include "sj_apkzip.h"
#include "sj_paths.h"
#include "opensles.h"
#include "config.h"
#include "sj_threads.h"
#include "sj_trace.h"

#define CH            2
#define RING_FRAMES   32768                 /* ~680 ms at 48 kHz */
#define CHUNK_FRAMES  1024                  /* decode granularity */

static char   g_asset_dir[256];
static char   g_current[128];
static int    g_ready;
static float  g_volume = 1.0f;
static float  g_music_gain = 1.0f;   /* what the engine last asked for */
static int    g_paused;

/* --- ring ---------------------------------------------------------------- */
static int16_t g_ring[RING_FRAMES * CH];
static int     g_head, g_tail;              /* frame indices, head==tail: empty */
static Mutex   g_lock;
static CondVar g_space;                     /* worker waits here when full     */

/* --- worker -------------------------------------------------------------- */
static Thread     g_thread;
static bool       g_thread_live;
static volatile bool g_stop;
static SjDecoder *g_dec;                    /* worker-owned once started */
static int        g_loop;
static volatile int g_finished;             /* stream ended and ring drained */

static int ring_count(void)
{
    int n = g_tail - g_head;
    return n < 0 ? n + RING_FRAMES : n;
}

static void ring_reset(void) { g_head = g_tail = 0; }

/* Worker: decode into the ring until told to stop. */
static void music_thread(void *arg)
{
    static int16_t chunk[CHUNK_FRAMES * CH];
    (void)arg;

    int empty_passes = 0;

    while (!g_stop) {
        sj_mark("music decode");
        int got = sj_decoder_read(g_dec, chunk, CHUNK_FRAMES);

        if (got <= 0) {
            /* End of stream. A looping track rewinds -- but a track that
             * yields nothing at all (a decode error from the very first
             * packet) would rewind, yield nothing, rewind... a tight loop that
             * never blocks, owning its core. Give up after two empty passes. */
            if (g_loop && ++empty_passes < 3 && sj_decoder_rewind(g_dec)) {
                svcSleepThread(1000000ULL);           /* never a hard spin */
                continue;
            }
            if (empty_passes >= 3)
                printf("sj_music: '%s' decodes to nothing; stopping it\n", g_current);
            g_finished = 1;
            break;
        }
        empty_passes = 0;

        /* Push `got` frames, waiting for room. Never spin: the audio thread
         * signals g_space every time it drains. */
        {
            int written = 0;
            while (written < got && !g_stop) {
                int n;
                mutexLock(&g_lock);
                while (ring_count() >= RING_FRAMES - 1 && !g_stop)
                    condvarWaitTimeout(&g_space, &g_lock, 50000000ULL); /* 50 ms */
                if (g_stop) { mutexUnlock(&g_lock); break; }

                n = got - written;
                {
                    int room = RING_FRAMES - 1 - ring_count();
                    int lin  = RING_FRAMES - g_tail;   /* to the wrap point */
                    if (n > room) n = room;
                    if (n > lin)  n = lin;
                }
                if (n > 0) {
                    memcpy(g_ring + (size_t)g_tail * CH,
                           chunk + (size_t)written * CH,
                           (size_t)n * CH * sizeof(int16_t));
                    g_tail = (g_tail + n) % RING_FRAMES;
                    written += n;
                }
                mutexUnlock(&g_lock);
            }
        }
    }
    g_thread_live = false;
}

/* Pull callback. Audio thread: memcpy and a multiply, nothing else. */
static int music_pull(void *ctx, int16_t *dst, int frames)
{
    int done = 0;
    (void)ctx;

    if (!g_ready || g_paused) return 0;

    mutexLock(&g_lock);
    while (done < frames) {
        int avail = ring_count();
        int lin, n;
        if (avail <= 0) break;
        lin = RING_FRAMES - g_head;
        n = frames - done;
        if (n > avail) n = avail;
        if (n > lin)   n = lin;
        memcpy(dst + (size_t)done * CH, g_ring + (size_t)g_head * CH,
               (size_t)n * CH * sizeof(int16_t));
        g_head = (g_head + n) % RING_FRAMES;
        done += n;
    }
    mutexUnlock(&g_lock);
    condvarWakeAll(&g_space);            /* room freed: let the worker run */

    if (g_volume < 0.999f) {
        int i, total = done * CH;
        for (i = 0; i < total; i++) dst[i] = (int16_t)(dst[i] * g_volume);
    }
    return done;
}

void sj_music_init(const char *asset_dir)
{
    mutexInit(&g_lock);
    condvarInit(&g_space);
    snprintf(g_asset_dir, sizeof(g_asset_dir), "%s", asset_dir);
    g_volume = config.music_volume / 100.0f;
    g_ready = 1;
    opensles_set_music_source(music_pull, NULL);
}

static void stop_worker(void)
{
    if (!g_thread_live) return;
    g_stop = true;
    condvarWakeAll(&g_space);            /* release it if it is waiting for room */
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_thread_live = false;
}

/* The track as stored in the APK: assets/<name>. The .m4a entries are STORED,
 * so ffmpeg reads them in place through a byte-range window; a deflated entry
 * (not expected, but harmless to support) is inflated into memory first. */
static SjDecoder *open_from_apk(const char *base, char *out, size_t cap,
                                SjDecodeInfo *info)
{
    const char *apk = sj_apk();
    char entry[200];
    SjZipEntry e;
    int rate = opensles_output_rate();

    if (!apk || !*apk) return NULL;
    snprintf(entry, sizeof(entry), "assets/%s", base);
    if (sj_zip_find(apk, entry, &e) != 0) return NULL;

    snprintf(out, cap, "%s!/%s", apk, entry);
    if (e.method == SJ_ZIP_STORED)
        return sj_decoder_open_range(apk, (int64_t)e.data_offset,
                                     (int64_t)e.size, rate, info);
    {
        size_t len = 0;
        void *buf = sj_zip_read_alloc(apk, &e, &len);
        return buf ? sj_decoder_open_mem(buf, len, rate, info) : NULL;
    }
}

/* The engine names files ".m4a". Try a loose override first, then the APK.
 * Returns the decoder and fills `out` with what was opened. */
static SjDecoder *open_track(const char *name, char *out, size_t cap,
                             SjDecodeInfo *info)
{
    static const char *exts[] = { ".m4a", ".ogg", ".mp3", ".wav", ".aac" };
    char stem[128];
    char tried[512];
    const char *base = strrchr(name, '/');
    char *dot;
    size_t i;
    int rate = opensles_output_rate();
    SjDecoder *d;

    base = base ? base + 1 : name;

    /* A loose file with the name as given. */
    snprintf(out, cap, "%s/%s", g_asset_dir, base);
    d = sj_decoder_open(out, rate, info);
    if (d) return d;

    snprintf(stem, sizeof(stem), "%s", base);
    dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    tried[0] = '\0';
    for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        snprintf(out, cap, "%s/%s%s", g_asset_dir, stem, exts[i]);
        d = sj_decoder_open(out, rate, info);
        if (d) return d;
        strncat(tried, exts[i], sizeof(tried) - strlen(tried) - 1);
        strncat(tried, " ",     sizeof(tried) - strlen(tried) - 1);
    }

    /* The normal case: straight out of the APK. */
    d = open_from_apk(base, out, cap, info);
    if (d) return d;

    /* Name everywhere we looked: "no sound" is usually either a missing file
     * or a wrong directory, and this distinguishes them without another
     * round trip. */
    printf("sj_music: '%s' not found as assets/%s in the APK (%s), nor loose "
           "in %s (tried %s)\n", base, base,
           sj_apk()[0] ? sj_apk() : "no APK found", g_asset_dir, tried);
    return NULL;
}

void sj_music_play(const char *name, int loop)
{
    char path[512];
    SjDecodeInfo info;
    SjDecoder *d;

    if (!g_ready || !name || !*name) return;

    /* The engine re-requests the current track on scene changes; restarting it
     * would audibly reset the loop. */
    if (g_thread_live && !g_finished && !strcmp(g_current, name)) return;

    sj_music_stop();

    memset(&info, 0, sizeof(info));
    d = open_track(name, path, sizeof(path), &info);
    if (!d) return;   /* open_track has already explained what it tried */
    printf("sj_music: %s [%s %d Hz %dch]\n", path,
           info.codec_name ? info.codec_name : "?", info.src_rate,
           info.src_channels);

    g_dec      = d;
    g_loop     = loop;
    /* Re-apply the engine's gain: g_volume may have been left at zero by a
     * mute during loading, and nothing else would restore it. */
    g_volume   = g_music_gain * (config.music_volume / 100.0f);
    g_paused   = 0;
    g_stop     = false;
    g_finished = 0;
    ring_reset();
    snprintf(g_current, sizeof(g_current), "%s", name);

    /* One notch BELOW the main thread, on a core of its own where possible.
     *
     * This used to be 0x2C on the default core, commented as "a notch below
     * the main thread" -- but 0x2C usually IS the main thread's priority, and
     * Horizon does not time-slice equal priorities. Anything that kept this
     * worker busy would have frozen our main loop, and HOME with it. The ring
     * holds ~680 ms, so a lower priority costs nothing: the main loop sleeps
     * most of every frame. */
    {
        const int below = sj_threads_main_prio() + 1;
        const int prios[] = { below < 0x3B ? below : 0x3B, 0x2D, 0x3B };
        if (sj_threads_create(&g_thread, music_thread, NULL, 0x20000, prios, 3,
                              sj_threads_aux_core(), "music decode") < 0) {
            printf("sj_music: could not start the decode thread\n");
            sj_decoder_close(g_dec);
            g_dec = NULL;
            g_current[0] = '\0';
            return;
        }
    }
    threadStart(&g_thread);
    g_thread_live = true;
}

void sj_music_stop(void)
{
    if (!g_ready) return;
    stop_worker();
    mutexLock(&g_lock);
    ring_reset();
    mutexUnlock(&g_lock);
    if (g_dec) { sj_decoder_close(g_dec); g_dec = NULL; }
    g_current[0] = '\0';
    g_finished = 0;
    g_paused = 0;
}

void sj_music_pause(void)  { g_paused = 1; }
void sj_music_resume(void) { g_paused = 0; }

int sj_music_is_playing(void)
{
    return g_ready && !g_paused && g_current[0] != '\0' &&
           (!g_finished || ring_count() > 0);
}

void sj_music_set_volume(float v)
{
    /* Some callers express volume as a percentage. Accept either rather than
     * silently clamping 100 down to 1.0 and sounding identical to 0..1 -- or
     * worse, clamping a legitimate 0..1 value that arrived as 0..100. */
    if (v > 1.0f && v <= 100.0f) v /= 100.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_music_gain = v;
    g_volume = g_music_gain * (config.music_volume / 100.0f);
}

void sj_music_exit(void)
{
    if (!g_ready) return;
    opensles_set_music_source(NULL, NULL);   /* audio thread stops calling us */
    sj_music_stop();
    g_ready = 0;
}

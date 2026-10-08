/* sj_trace.c -- runtime counters and a watchdog heartbeat.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * A hang is much harder to diagnose than a crash: there is no report, no PC,
 * and the log simply stops. These counters exist so the log keeps saying
 * something useful while nothing appears to be happening.
 *
 * The heartbeat runs on its own thread and prints one line every few seconds:
 *
 *   sj: [ 6s] swaps=372(+60/s) polls=21440 jni=8 opens=141 fail=0 clock=+3.00s
 *
 * Reading it:
 *
 *   swaps advancing            -> the render loop is alive
 *   swaps advancing, clock flat-> gettimeofday is stuck; appUpdate is starved
 *   swaps flat, polls advancing-> the glue is spinning in its poll loop and
 *                                 never reaching the frame (the classic
 *                                 "pollOnce never returns negative" bug)
 *   both flat                  -> the game thread is blocked; check the
 *                                 cond-wait and file-open warnings
 *   opens climbing, fail > 0   -> it is hunting for a file it cannot find
 *
 * All counters are plain non-atomic ints. They are advisory: a torn read costs
 * nothing and atomics on every swap would be a real cost for a debug aid.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "sj_trace.h"
#include "sj_log.h"
#include "sj_input.h"
#include "sj_sensor.h"
#include "sj_threads.h"

unsigned sj_n_swaps;
unsigned sj_n_polls;
unsigned sj_n_jni;
unsigned sj_n_opens;
unsigned sj_n_open_fail;
unsigned sj_n_threads;
unsigned sj_n_cond_waits;
unsigned sj_n_touches;
unsigned sj_n_sensor_samples;
double   sj_last_clock;

/* --- per-thread breadcrumbs ------------------------------------------------
 * The counters say the game stopped; they cannot say where. Each instrumented
 * shim records "thread X last entered Y at time T", and the heartbeat prints
 * the table. When everything stalls, the thread that is stuck shows an age
 * that keeps growing and names the exact call it is sitting in.
 *
 * Deliberately lock-free and fixed-size: this runs inside hot shims, and a
 * mutex here would change the timing of the very thing being measured. Worst
 * case two threads share a slot and one breadcrumb is wrong, which is a fair
 * trade for zero contention.
 * ------------------------------------------------------------------------ */
#define MARK_SLOTS 12

typedef struct {
    uint32_t    tid;
    const char *what;
    uint64_t    when;
    unsigned    hits;
} Mark;

static Mark g_marks[MARK_SLOTS];

void sj_mark(const char *what)
{
    uint32_t tid = (uint32_t)(uintptr_t)armGetTls();
    int i, free_slot = -1;

    for (i = 0; i < MARK_SLOTS; i++) {
        if (g_marks[i].tid == tid) {
            g_marks[i].what = what;
            g_marks[i].when = armTicksToNs(armGetSystemTick());
            g_marks[i].hits++;
            return;
        }
        if (free_slot < 0 && g_marks[i].tid == 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_marks[free_slot].what = what;
        g_marks[free_slot].when = armTicksToNs(armGetSystemTick());
        g_marks[free_slot].hits = 1;
        g_marks[free_slot].tid  = tid;   /* published last */
    }
}

static void dump_marks(void)
{
    uint64_t now = armTicksToNs(armGetSystemTick());
    int i;
    for (i = 0; i < MARK_SLOTS; i++) {
        if (!g_marks[i].tid) continue;
        /* Signed: the marking thread can update `when` after we sampled
         * `now`, and an unsigned subtraction then prints 18446744073s. */
        int64_t age = (int64_t)now - (int64_t)g_marks[i].when;
        if (age < 0) age = 0;
        printf("sj:        thread %08x last in %-26s %6.2fs ago (%u hits)\n",
               g_marks[i].tid, g_marks[i].what ? g_marks[i].what : "?",
               (double)age / 1e9, g_marks[i].hits);
    }
}

static char   g_last_open[256];
static char   g_last_fail[256];
static char   g_last_jni[192];
static Thread g_thread;
static bool   g_live;
static volatile bool g_stop;

void sj_trace_open(const char *path, int ok)
{
    sj_n_opens++;
    if (path) {
        snprintf(g_last_open, sizeof(g_last_open), "%s", path);
        if (!ok) {
            sj_n_open_fail++;
            snprintf(g_last_fail, sizeof(g_last_fail), "%s", path);
        }
    }
}

void sj_trace_jni(const char *what)
{
    sj_n_jni++;
    if (what) snprintf(g_last_jni, sizeof(g_last_jni), "%s", what);
}

/* --- watchdog ---------------------------------------------------------------
 * The first hardware run of this port froze on the menu with the console
 * unresponsive to HOME and the log simply stopping. A hang leaves nothing to
 * read; a crash leaves an Atmosphere report with EVERY thread's registers and
 * backtrace. So when progress stops for long enough, log what we know, flush
 * it to the card, and break deliberately (pvzultimate's watchdog does the same
 * and it is how its deadlocks were found).
 *
 * Two kinds of progress are watched:
 *   - our main loop, which beats every iteration (it only ever sleeps 2 ms or
 *     16 ms, so 15 s without a beat is never legitimate), and
 *   - the game's rendering, counted in eglSwapBuffers, while the app has focus
 *     (the engine draws every frame, loading screens included).
 * This thread runs at a priority above the game's and the main thread's, on
 * a core of its own where possible: a watchdog that a spinning thread can
 * starve fires in exactly the situations where it is not needed. */
#define WD_MAIN_WARN_NS    5000000000ULL
#define WD_MAIN_BREAK_NS  15000000000ULL
#define WD_RENDER_WARN_NS 10000000000ULL
#define WD_RENDER_BREAK_NS 30000000000ULL

static volatile uint64_t g_main_beat_ns;
static volatile int      g_focused = 1;
static volatile int      g_wd_armed = 1;
static unsigned          g_wd_swaps;
static uint64_t          g_wd_swaps_ns;
static int               g_warned_main, g_warned_render;
static int               g_verbose;        /* print the 3 s status lines */

static uint64_t ns_now(void) { return armTicksToNs(armGetSystemTick()); }

void sj_wd_main_beat(void) { g_main_beat_ns = ns_now(); g_warned_main = 0; }

void sj_wd_set_focus(int focused)
{
    g_focused = focused;
    g_wd_swaps_ns = ns_now();      /* do not count time spent in the background */
    g_warned_render = 0;
}

void sj_wd_disarm(void) { g_wd_armed = 0; }

static void wd_stall(const char *what, uint64_t ns)
{
    printf("\n================================================================\n"
           "sj: STALL: %s has made no progress for %.1f s\n"
           "================================================================\n",
           what, (double)ns / 1e9);
    printf("sj:   swaps=%u polls=%u jni=%u opens=%u fail=%u threads=%u "
           "condwaits=%u\n", sj_n_swaps, sj_n_polls, sj_n_jni, sj_n_opens,
           sj_n_open_fail, sj_n_threads, sj_n_cond_waits);
    printf("sj:   breadcrumbs (a thread whose age keeps growing is stuck in the "
           "call named):\n");
    dump_marks();
    if (g_last_jni[0])  printf("sj:   last JNI: %s\n", g_last_jni);
    if (g_last_open[0]) printf("sj:   last open: %s\n", g_last_open);
}

static void wd_check(void)
{
    const uint64_t now = ns_now();
    const unsigned swaps = sj_n_swaps;

    if (!g_wd_armed) return;

    if (g_main_beat_ns) {
        const uint64_t idle = now - g_main_beat_ns;
        if (idle > WD_MAIN_BREAK_NS) {
            wd_stall("the main loop", idle);
            goto trip;
        }
        if (idle > WD_MAIN_WARN_NS && !g_warned_main) {
            g_warned_main = 1;
            wd_stall("the main loop", idle);
            sj_log_flush();
        }
    }

    if (swaps != g_wd_swaps || !g_wd_swaps_ns) {
        g_wd_swaps = swaps;
        g_wd_swaps_ns = now;
        g_warned_render = 0;
    } else if (g_focused && swaps > 0) {
        const uint64_t idle = now - g_wd_swaps_ns;
        if (idle > WD_RENDER_BREAK_NS) {
            wd_stall("rendering (eglSwapBuffers)", idle);
            goto trip;
        }
        if (idle > WD_RENDER_WARN_NS && !g_warned_render) {
            g_warned_render = 1;
            wd_stall("rendering (eglSwapBuffers)", idle);
            sj_log_flush();
        }
    }
    return;

trip:
    printf("sj: breaking deliberately so Atmosphere writes a crash report with "
           "every thread's backtrace (sdmc:/atmosphere/crash_reports/). Resolve "
           "it with tools/resolve_crash.py. Set watchdog=0 in config.txt to "
           "disable this.\n");
    sj_log_flush();
    g_wd_armed = 0;
    svcBreak(BreakReason_Panic, 0, 0);
}

/* One line: the counters that move during boot, then every thread's latest
 * breadcrumb with its age in ms. */
static void boot_line(uint64_t t0)
{
    const uint64_t now = ns_now();
    char marks[512];
    size_t o = 0;
    int i;
    marks[0] = 0;
    for (i = 0; i < MARK_SLOTS && o < sizeof(marks) - 1; i++) {
        if (!g_marks[i].tid) continue;
        int64_t age = (int64_t)now - (int64_t)g_marks[i].when;
        if (age < 0) age = 0;
        int n = snprintf(marks + o, sizeof(marks) - o, " %04x:%s+%lld",
                         g_marks[i].tid & 0xFFFF,
                         g_marks[i].what ? g_marks[i].what : "?",
                         (long long)(age / 1000000));
        if (n < 0) break;
        o += (size_t)n;
    }
    printf("sj: [b%6.2fs] swaps=%u polls=%u jni=%u opens=%u/%u thr=%u "
           "audio=%u/%u/%u sens=%u touch=%u |%s\n",
           (double)(now - t0) / 1e9, sj_n_swaps, sj_n_polls, sj_n_jni,
           sj_n_opens, sj_n_open_fail, sj_n_threads, sj_n_audio_cb,
           sj_n_enqueued, sj_n_music_frames, sj_n_sensor_samples, sj_n_touches,
           marks);
}

static void heartbeat(void *arg)
{
    unsigned last_swaps = 0, last_polls = 0;
    double   last_clock = 0.0;
    int      secs = 0;
    int      quiet = 0;
    const int period = 3;
    (void)arg;

    /* Boot trace. The orange screen (an external abort: Atmosphere's secure
     * monitor stops the console the instant it happens) has struck within the
     * first seconds -- within 250 ms of the store switching on in the build-3
     * trace. A line every 100 ms for the first 30 s puts the last one written
     * within a tenth of a second of it; every line is on the card before the
     * next. */
    if (g_verbose) {
        const uint64_t t0 = ns_now();
        for (int i = 0; i < 300 && !g_stop; i++) {
            svcSleepThread(100000000ULL);
            boot_line(t0);
            if (i % 10 == 9) wd_check();
        }
        secs = 30;
    } else {
        /* Give the game a moment to get going. */
        svcSleepThread(1000000000ULL); wd_check();
        svcSleepThread(1000000000ULL); wd_check();
    }

    while (!g_stop) {
        unsigned swaps = sj_n_swaps, polls = sj_n_polls;
        double   clock = sj_last_clock;
        secs += period;

        if (!g_verbose) {
            int s;
            for (s = 0; s < period && !g_stop; s++) {
                svcSleepThread(1000000000ULL);
                wd_check();
            }
            continue;
        }

        printf("sj: [%3ds] swaps=%u(+%u) polls=%u(+%u) jni=%u opens=%u "
               "fail=%u threads=%u condwaits=%u touch=%u/%u clock=%+.3fs\n",
               secs, swaps, swaps - last_swaps, polls, polls - last_polls,
               sj_n_jni, sj_n_opens, sj_n_open_fail, sj_n_threads,
               sj_n_cond_waits, sj_n_touches, sj_input_finishes(),
               clock - last_clock);

        /* touch=injected/finished. The second number is the one that matters:
         * it counts events the game actually consumed. Injected climbing while
         * finished stays flat means input is reaching our queue but not the
         * game -- and the splash screen only advances on a touch. */
        printf("sj:        input reads by the game: getType=%u getAction=%u "
               "getXY=%u\n", sj_n_getType, sj_n_getAction, sj_n_getXY);
        printf("sj:        sensor: queue ident=%d, samples delivered=%u\n",
               sj_sensor_looper_ident(), sj_n_sensor_samples);
        if (!sj_n_sensor_samples)
            printf("sj:        -> the game has never collected a tilt sample; "
                   "steering cannot work\n");
        printf("sj:        audio: cb=%u voices=%u enqueued=%u musicframes=%u "
               "peak=%u\n", sj_n_audio_cb, sj_n_voices_live, sj_n_enqueued,
               sj_n_music_frames, sj_audio_peak);
        if (!sj_n_audio_cb)
            printf("sj:        -> the audio callback has never run: the device "
                   "is not open (look for the [sl] open/FAILED line)\n");
        else if (!sj_audio_peak && (sj_n_enqueued || sj_n_music_frames))
            printf("sj:        -> mixer running and fed, but the output is "
                   "silent: check voice play state and volume\n");
        else if (sj_n_audio_cb && !sj_n_enqueued && !sj_n_music_frames)
            printf("sj:        -> mixer running but nothing is feeding it: no "
                   "SFX enqueued and no music pulled\n");
        if (sj_n_touches && !sj_input_finishes())
            printf("sj:        -> touches queued but NONE consumed by the game; "
                   "the splash needs one to advance\n");
        else if (sj_input_finishes() && !sj_n_getAction)
            printf("sj:        -> events are being DRAINED but the game never "
                   "reads them: app->onInputEvent is not wired up\n");
        if (sj_input_dispatch_stall() > 64)
            printf("sj:        -> %u input reports with nothing finished\n",
                   sj_input_dispatch_stall());

        /* Say the diagnosis out loud rather than making it be inferred from
         * six numbers at three in the morning. */
        if (swaps == last_swaps && polls == last_polls) {
            printf("sj:        -> render AND poll both stalled: the game "
                   "thread is blocked, not looping. The thread whose age "
                   "keeps growing below is the one that is stuck.\n");
        } else if (swaps == last_swaps && polls != last_polls) {
            printf("sj:        -> polling but not drawing: the glue is stuck "
                   "in its inner poll loop and never reaches appUpdate\n");
        } else if (clock == last_clock) {
            printf("sj:        -> drawing but the clock is flat: the frame "
                   "accumulator is starved, appUpdate is not being called\n");
        }

        /* The breadcrumb table is the whole point once something stalls:
         * a thread whose age keeps growing is sitting in the call named. */
        dump_marks();

        if (g_last_fail[0])
            printf("sj:        last failed open: %s\n", g_last_fail);
        if (g_last_jni[0])
            printf("sj:        last JNI: %s\n", g_last_jni);
        if (g_last_open[0] && ++quiet % 4 == 0)
            printf("sj:        last open: %s\n", g_last_open);

        sj_log_flush();
        last_swaps = swaps;
        last_polls = polls;
        last_clock = clock;
        {
            int s;
            for (s = 0; s < period && !g_stop; s++) {
                svcSleepThread(1000000000ULL);
                wd_check();
            }
        }
    }
    g_live = false;
}

void sj_trace_start(int verbose)
{
    /* Above the main thread and the game's threads, so a spinner cannot
     * starve it. It was 0x3B -- the lowest band -- on the reasoning that it
     * must never displace the game, which is exactly backwards for a watchdog
     * on a strictly priority-ordered scheduler (pvzultimate learned this the
     * same way). It sleeps a second at a time, so it costs nothing. The list
     * walks down because hbloader refuses priorities far from 0x2C
     * (sonicolympics_nx: 0x18 -> 0xe001). */
    static const int prios[] = { 0x28, 0x2A, 0x2B, 0x2C };

    if (g_live) return;
    g_stop = false;
    g_verbose = verbose;
    g_main_beat_ns = ns_now();
    if (sj_threads_create(&g_thread, heartbeat, NULL, 0x8000, prios,
                          (int)(sizeof(prios) / sizeof(prios[0])),
                          sj_threads_aux_core(), "watchdog") < 0)
        return;
    threadStart(&g_thread);
    g_live = true;
    printf("sj: watchdog armed (main loop: break after %llus; rendering: "
           "break after %llus)%s\n",
           (unsigned long long)(WD_MAIN_BREAK_NS / 1000000000ULL),
           (unsigned long long)(WD_RENDER_BREAK_NS / 1000000000ULL),
           verbose ? "; heartbeat line every 3s" : "");
}

void sj_trace_stop(void)
{
    if (!g_live) return;
    g_stop = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_live = false;
}

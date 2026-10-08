/* sj_glue.c -- definitions the inherited shim layer references but no longer has.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * libc_shim.c, jni_fake.c and opensles.c came from ports that had a Unity
 * object model, a Journey feature layer, a Firebase stub and a Cocos video
 * player alongside them. Dropping those files leaves a handful of dangling
 * references. Every one is defined here, deliberately in one place, so it is
 * obvious what is inherited scaffolding rather than Sonic Jump behaviour.
 *
 * These were found by resolving the whole tree's symbols against itself rather
 * than by reading: each is a clean compile and a link failure, which is the
 * worst place to discover them.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <switch.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "android_native.h"
#include "sj_ndk.h"      /* ANativeActivity_finish prototype: a mismatch here
                          * compiles clean and corrupts the call site */
#include "sj_sensor.h"
#include "util.h"        /* debugLogNote */
#include "sj_glue.h"
#include "sj_log.h"
#include "sj_trace.h"

/* ---------------------------------------------------------------------------
 * ANativeActivity_finish
 *
 * The engine calls this when the player quits from its own menu.
 *
 * Do NOT exit() here. The glue expects to return from this call, finish its
 * frame, and be driven through onPause -> onStop -> onDestroy by us. Exiting
 * from inside game code skips FINI_ARRAY (the EASTL and tinyxml2 destructors)
 * and, far more visibly, skips the save-on-exit write in onPause -- the player
 * silently loses progress.
 * ------------------------------------------------------------------------ */
static volatile int g_finish_requested;
static volatile int g_quit_via_exit;

void ANativeActivity_finish(ANativeActivity *activity)
{
    (void)activity;
    printf("sj: ANativeActivity_finish -- quit requested by the game\n");
    g_finish_requested = 1;
}

int sj_finish_requested(void) { return g_finish_requested; }

/* Set by the exit() shim in imports.c. The distinction matters at teardown:
 * after exit() the calling thread is parked (see sj_exit_park below), so
 * driving the activity lifecycle would block forever waiting for a thread that
 * is never coming back. main() checks this and skips the callbacks. */
int sj_quit_via_exit(void) { return g_quit_via_exit; }

/* The engine calls exit() from its quit path. We cannot let it kill the
 * process -- that skips FINI_ARRAY and, far more visibly, the save-on-exit
 * write in onPause. But we also cannot simply return: exit() is declared
 * noreturn, so the compiler emits nothing usable after the call and returning
 * runs whatever bytes follow.
 *
 * So: flag the quit, then park this thread. main() notices the flag, tears the
 * subsystems down and returns, and libnx ends the process -- taking the parked
 * thread with it. */
void sj_exit_park(void)
{
    g_quit_via_exit   = 1;
    g_finish_requested = 1;
    printf("sj: exit() intercepted -- parking this thread for teardown\n");
    for (;;) svcSleepThread(100000000ULL);   /* 100 ms */
}

/* ---------------------------------------------------------------------------
 * android_get_orientation
 *
 * jni_fake.c offers this to Java-side callers that ask the activity for device
 * orientation. Sonic Jump's steering goes through the ASensor path
 * (sj_sensor.c), so both must report the same vector or tilt would disagree
 * with itself depending on which route the engine took.
 * ------------------------------------------------------------------------ */
void android_get_orientation(float *x, float *y, float *z)
{
    sj_sensor_get(x, y, z);
}

void android_set_orientation(float x, float y, float z)
{
    /* Nothing writes orientation from outside; the sensor layer owns it. */
    (void)x; (void)y; (void)z;
}

/* ---------------------------------------------------------------------------
 * android_native_draw_cursor
 *
 * The Unity ports drew a docked-mode pointer as a GL overlay from inside their
 * eglSwapBuffers wrapper. Sonic Jump renders on the glue thread and we do not
 * wrap its swap, so there is no safe point to issue GL from here. The docked
 * pointer is handled in main.c instead, by moving the touch point rather than
 * drawing over the frame.
 * ------------------------------------------------------------------------ */
void android_native_draw_cursor(void) { }

/* ---------------------------------------------------------------------------
 * sj_gettimeofday
 *
 * android_main drives the game from a fixed-timestep accumulator:
 *
 *     now  = tv.tv_sec + tv.tv_usec / 1e6;
 *     acc += now - last;  last = now;
 *     while (acc >= 1/60) { appUpdate(1/60); acc -= 1/60; }
 *
 * so if this ever stops advancing, appUpdate stops being called and the game
 * freezes on whatever frame it last drew -- while still rendering, so it looks
 * like a hang rather than a stopped clock. That failure mode is invisible
 * without instrumentation, hence the wrapper.
 *
 * Logs the first few calls and then only complains if time goes backwards or
 * stalls, so it costs nothing once things are working.
 * ------------------------------------------------------------------------ */
int sj_gettimeofday(struct timeval *tv, void *tz)
{
    static int    calls;
    static double last;
    static int    stalls;
    int rc = gettimeofday(tv, tz);
    double now;

    if (!tv) return rc;
    now = (double)tv->tv_sec + (double)tv->tv_usec / 1000000.0;

    if (calls < 5) {
        printf("sj: gettimeofday #%d -> %lld.%06ld (rc=%d)\n",
               calls, (long long)tv->tv_sec, (long)tv->tv_usec, rc);
        if (calls == 4)
            printf("sj: (further gettimeofday calls are silent unless the "
                   "clock stalls)\n");
    } else if (now <= last) {
        if (++stalls <= 5)
            printf("sj: WARNING clock did not advance (%.6f -> %.6f). The frame "
                   "accumulator needs this to move or appUpdate is never "
                   "called.\n", last, now);
    }
    calls++;
    last = now;
    sj_last_clock = now;      /* heartbeat: is the frame clock advancing? */
    return rc;
}

/* ---------------------------------------------------------------------------
 * sj_clock -- bionic-compatible clock()
 *
 * The engine's whole timing layer runs on this:
 *
 *     slGetSystemTimer()                 -> clock()
 *     slSystemTimerDeltaToSeconds(d)     -> d / 1000000.0
 *
 * so it assumes CLOCKS_PER_SEC == 1000000, which is what bionic (and glibc)
 * give. devkitA64's newlib does not: CLOCKS_PER_SEC is 1000 there, and its
 * clock() is backed by times(), which on Horizon has no process CPU accounting
 * to report and returns a value that does not advance.
 *
 * Passing newlib's clock() straight through therefore hands the engine a timer
 * that is either a thousand times too slow or frozen. AppLoadingScreen::update
 * paces its load steps against exactly this timer, which is why the game sat on
 * the logo rendering happily at 60 fps while never advancing a step.
 *
 * Return monotonic wall time in microseconds. The engine only ever uses
 * differences, so the epoch is irrelevant; what matters is the unit and that it
 * moves.
 * ------------------------------------------------------------------------ */
clock_t sj_clock(void)
{
    static int logged;
    uint64_t us = armTicksToNs(armGetSystemTick()) / 1000ULL;

    if (logged < 3) {
        printf("sj: clock() -> %llu us (CLOCKS_PER_SEC assumed 1000000 by the "
               "engine; newlib says %ld)\n",
               (unsigned long long)us, (long)CLOCKS_PER_SEC);
        logged++;
    }
    return (clock_t)us;
}

/* ---------------------------------------------------------------------------
 * debugLogNote -- opensles.c and mp3_decode.c log through this.
 * ------------------------------------------------------------------------ */
int debugLogNote(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    /* Straight to the log file and flushed: opensles.c and mp3_decode.c use
     * this for the errors that tend to precede a crash, where a line sitting
     * in a buffer would be lost. */
    sj_log_write(line, (size_t)n);
    sj_log_flush();
    return n;
}

/* ---------------------------------------------------------------------------
 * fake_unityplayer_thiz
 *
 * jni_fake.c compares receivers against Unity's UnityPlayer singleton. There is
 * no UnityPlayer here; a distinct non-NULL address means the comparison is
 * well-defined and always false.
 * ------------------------------------------------------------------------ */
static int g_no_unityplayer;
void *fake_unityplayer_thiz = &g_no_unityplayer;

/* ---------------------------------------------------------------------------
 * firebase_stub_lookup
 *
 * libc_shim.c routes dlsym() through this so Journey's Firebase library could
 * resolve against a stub table. Sonic Jump links no Firebase; returning 0 lets
 * dlsym fall through to the real module search in so_util.c.
 * ------------------------------------------------------------------------ */
void *firebase_stub_lookup(const char *name)
{
    (void)name;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * journey_keyboard_set_result
 *
 * jni_fake.c calls this when the Switch software keyboard closes. Sonic Jump
 * has no text entry -- there is no name-entry screen and no chat -- so the
 * result is discarded. If a build ever does raise the keyboard, this is where
 * to route the text back into the engine.
 * ------------------------------------------------------------------------ */
void journey_keyboard_set_result(const char *text, int canceled)
{
    (void)text; (void)canceled;
}

/* ---------------------------------------------------------------------------
 * mmap arena globals
 *
 * libc_shim.c's mmap emulation carves allocations out of an arena that the
 * host reserves. Journey needed a huge, specially aligned one because Unity
 * 2022 packs a 12-bit region index into pointer bits.
 *
 * Sonic Jump has no such constraint: it is a custom engine that mmaps nothing
 * unusual, and its largest single allocation is an 8 MB 2048x2048 texture
 * staging buffer. Leaving the arena disabled makes libc_shim fall back to
 * plain memalign, which is what we want.
 *
 * If a future build does need a real arena, set these from main() before the
 * module's INIT_ARRAY runs -- libc_shim reads them on first use.
 * ------------------------------------------------------------------------ */
void   *g_mmap_arena_base;   /* NULL = no arena, fall back to memalign */
size_t  g_mmap_arena_size;
size_t  g_mmap_big_align;    /* 0 = no special alignment requirement */
int     g_overcommit;        /* 0 = do not pretend allocations succeed */

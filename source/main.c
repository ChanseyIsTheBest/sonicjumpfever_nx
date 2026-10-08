/* main.c -- sonicjumpfever_nx entry point.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 * Derived from the sonicjump_nx wrapper, itself derived from the
 * angrybirdsjourney_nx / happywheels wrappers (C) 2021 Andy Nguyen, fgsfds.
 *
 * Sonic Jump Fever is a plain NativeActivity title on the same in-house SEGA /
 * Hardlight engine as Sonic Jump: one module, and the engine drives its own
 * loop once ANativeActivity_onCreate has been called. Our job is to
 *
 *   1. extract (first boot only), load and relocate libsonicjumpfever.so
 *   2. build a plausible ANativeActivity and hand it to onCreate
 *   3. pump HID into the input queue every frame
 *   4. drive the activity lifecycle on suspend/resume/quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "imports.h"
#include "util.h"
#include "error.h"
#include "jni_fake.h"
#include "android_native.h"
#include "sj_input.h"
#include "sj_sensor.h"
#include "sj_music.h"
#include "sj_jni.h"
#include "sj_glue.h"
#include "sj_paths.h"
#include "sj_log.h"
#include "sj_saveedit.h"
#include "sj_calltrace.h"
#include "sj_trace.h"
#include "nx_pointer.h"
#include "opensles.h"
#include "sj_apkzip.h"
#include "sj_threads.h"

so_module main_mod;
int screen_width  = 1280;
int screen_height = 720;

static PadState g_pad;
static int      g_sixaxis_ok;
static HidSixAxisSensorHandle g_sixaxis[4];

PadState *sj_pad(void) { return &g_pad; }

/* No __libnx_initheap override.
 *
 * An earlier draft reserved a 320 MB static array and pointed the heap at it.
 * That is worse than doing nothing: it puts 320 MB of .bss in the NRO for the
 * loader to map up front, and it caps us at 320 MB even when the console has
 * more free. libnx's default initheap calls svcSetHeapSize for everything
 * available (minus any envGetHeapOverride the loader set), which is both more
 * memory and less to go wrong.
 *
 * For scale: the engine's largest single allocation is an 8 MB staging buffer
 * for a 2048x2048 RGBA4444 page, and there are 11 pages that size.
 */

/* --- six-axis ------------------------------------------------------------- */

static int g_sixaxis_have[4];
static int g_sixaxis_started;

static void sixaxis_init(void)
{
    if (g_sixaxis_started) return;
    g_sixaxis_started = 1;
    /* Handheld and the various Joy-Con layouts expose different handles, so
     * grab them all and read whichever is actually delivering samples.
     *
     * The Results were previously ignored and every handle started blindly:
     * an acquisition that failed left a garbage handle that was then started
     * and polled for ever, with nothing anywhere saying the sensor was not
     * available. Check each one and say so. */
    Result rc;
    int i, ok = 0;

    rc = hidGetSixAxisSensorHandles(&g_sixaxis[0], 1, HidNpadIdType_Handheld,
                                    HidNpadStyleTag_NpadHandheld);
    g_sixaxis_have[0] = R_SUCCEEDED(rc);
    printf("sj: sixaxis handheld    %s\n", g_sixaxis_have[0] ? "ok" : "unavailable");

    rc = hidGetSixAxisSensorHandles(&g_sixaxis[1], 2, HidNpadIdType_No1,
                                    HidNpadStyleTag_NpadJoyDual);
    g_sixaxis_have[1] = g_sixaxis_have[2] = R_SUCCEEDED(rc);
    printf("sj: sixaxis joy-con x2  %s\n", g_sixaxis_have[1] ? "ok" : "unavailable");

    rc = hidGetSixAxisSensorHandles(&g_sixaxis[3], 1, HidNpadIdType_No1,
                                    HidNpadStyleTag_NpadFullKey);
    g_sixaxis_have[3] = R_SUCCEEDED(rc);
    printf("sj: sixaxis pro pad     %s\n", g_sixaxis_have[3] ? "ok" : "unavailable");

    for (i = 0; i < 4; i++) {
        if (!g_sixaxis_have[i]) continue;
        if (R_SUCCEEDED(hidStartSixAxisSensor(g_sixaxis[i]))) ok++;
    }
    g_sixaxis_ok = ok > 0;
    printf("sj: %d six-axis sensor(s) started; tilt_mode=%d "
           "(0=stick 1=gyro 2=both)\n", ok, config.tilt_mode);
    if (!g_sixaxis_ok)
        printf("sj: WARNING no six-axis sensor available; gyro tilt cannot "
               "work, the left stick will still steer\n");
}

int sj_read_sixaxis(HidSixAxisSensorState *out)
{
    if (!g_sixaxis_ok) return 0;
    sj_mark("sixaxis read");
    for (int i = 0; i < 4; i++) {
        HidSixAxisSensorState st;
        if (!g_sixaxis_have[i]) continue;
        memset(&st, 0, sizeof(st));
        if (hidGetSixAxisSensorStates(g_sixaxis[i], &st, 1) > 0) {
            /* A handle for a controller that is not attached still returns a
             * state, just an all-zero one. Real accelerometer output always
             * carries gravity somewhere, so a zero vector means this is not
             * the live handle -- keep looking rather than reporting no tilt. */
            float mag2 = st.acceleration.x * st.acceleration.x +
                         st.acceleration.y * st.acceleration.y +
                         st.acceleration.z * st.acceleration.z;
            if (mag2 < 0.04f) continue;      /* < 0.2 g total: not real */
            *out = st;
            {
                static int n;
                if (n < 3) {
                    printf("sj: sixaxis[%d] accel = (%.2f, %.2f, %.2f) g\n", i,
                           st.acceleration.x, st.acceleration.y, st.acceleration.z);
                    n++;
                }
            }
            return 1;
        }
    }
    return 0;
}

/* --- touch pumping --------------------------------------------------------
 * Handheld gets the real touchscreen. Docked gets a stick-driven cursor,
 * because there is no other way to point at a UI built for fingers.
 * ------------------------------------------------------------------------ */

static int      g_touch_down;

/* Pointer input.
 *
 * nx_pointer owns the pad, the touchscreen, a USB mouse and the stick-driven
 * cursor, and draws the cursor overlay. It replaces the hand-rolled pump_touch
 * that lived here, which mixed coordinate spaces and had no mouse support.
 *
 * Gyro pointing is compiled out (NXP_NO_GYRO): the six-axis sensors are read by
 * sj_sensor.c and reported to the game as the Android accelerometer, because
 * that is how Sonic Jump Fever steers. Pointing and steering with the same
 * motion would be unusable, and two consumers cannot share the handles.
 */
/* One pointer at a time.
 *
 * nx_pointer reports true multitouch: ids 0..7 for fingers plus id 8 for the
 * stick cursor. Fever's handler understands pointer ids, but everything it
 * needs -- tap to jump, tap menu buttons -- is a single finger, and feeding it
 * several sources at once (touchscreen, stick cursor, jump buttons) as
 * separate pointers would let a menu button see a press begin on one and end
 * on another. So the first pointer to go down owns the stream until it
 * releases, and goes to the game as pointer 0; everything else is dropped.
 * That is what a phone with one thumb on it delivers.
 */
/* Pointer id for the face-button jump. Outside nx_pointer's range (touch slots
 * 0..7, cursor 8) so it arbitrates against them like any other pointer. */
static char g_cfg_path[600];   /* kept so the in-game toggles can persist */
static uint64_t g_sens_save_at;/* deferred config write; see pump_touch */

#define JUMP_PTR_ID 100
/* B joins the jump set only when it is not Back: otherwise backing out of a
 * menu would also tap the centre of the screen. */
#define JUMP_BUTTONS (HidNpadButton_A | HidNpadButton_X | HidNpadButton_Y | \
                      (config.back_button ? 0 : HidNpadButton_B))

/* All four shoulders at once: flips tilt_invert, in-game, and saves it. */
#define TILT_INVERT_COMBO (HidNpadButton_L  | HidNpadButton_R | \
                           HidNpadButton_ZL | HidNpadButton_ZR)

static int      g_ptr_owner = -1;      /* nxp id currently driving the game */
static uint64_t g_ptr_down_ns;
static uint64_t g_ptr_move_ns;
static int      g_ptr_up_pending;
static float    g_ptr_up_x, g_ptr_up_y;

/* Long enough that a press always spans several game frames. appUpdate drains
 * the whole queued batch into TouchMonitor and then runs the screen update, so
 * a DOWN and its UP arriving together would go active and inactive within one
 * frame and read as no touch at all. */
#define TOUCH_MIN_HOLD_NS  80000000ULL
#define TOUCH_MOVE_MIN_NS   8000000ULL   /* cap MOVE at ~120 Hz */

static void deliver(int32_t action, float x, float y)
{
    android_window_to_game(&x, &y);
    sj_inject_touch(action, x, y);
}

static void pump_touch(void)
{
    NxpEvent ev[24];
    const uint64_t now = armTicksToNs(armGetSystemTick());
    int n, i;

    nxp_update();
    n = nxp_poll(ev, (int)(sizeof(ev) / sizeof(ev[0])));

    /* L + R + ZL + ZR together flips the tilt direction.
     *
     * All four shoulders at once is not a grip anyone falls into by accident,
     * and it is reachable mid-level without a menu -- which matters because
     * whether tilt steers the right way depends on how the console is being
     * held, and that is exactly the sort of thing you only discover once Sonic
     * is already falling.
     *
     * ZL and ZR normally tap at the cursor, so pointer input is dropped while
     * the combo is held; otherwise flipping the tilt would also poke whatever
     * the cursor was sitting on. The neutral pose is deliberately NOT
     * recalibrated: inversion mirrors the steering about the existing centre,
     * which is more predictable than silently moving it. */
    /* D-pad left / right trims the left stick's steering range.
     *
     * How far the stick leans Sonic is taste, and the useful value depends on
     * whether you are also tilting -- so it belongs on the pad, next to the
     * stick, not in a file. Up/down already adjusts the cursor inside
     * nx_pointer, which leaves left/right free.
     *
     * Only the stick is scaled. Motion steering is a physical tilt and there
     * is nothing to scale about it: the gravity vector is what it is.
     *
     * The save is deferred a second past the last press. Tuning this means a
     * burst of ten or twenty presses, and writing config.txt on every one of
     * them would be twenty SD-card writes for one decision. */
    {
        const u64 down = padGetButtonsDown(&g_pad);
        int step = 0;

        if (down & HidNpadButton_Right) step =  SJ_STICK_SENS_STEP;
        else if (down & HidNpadButton_Left) step = -SJ_STICK_SENS_STEP;

        if (step && config.tilt_mode != TILT_GYRO) {
            int v = config.stick_sens + step;
            if (v < SJ_STICK_SENS_MIN) v = SJ_STICK_SENS_MIN;
            if (v > SJ_STICK_SENS_MAX) v = SJ_STICK_SENS_MAX;
            if (v != config.stick_sens) {
                config.stick_sens = v;
                g_sens_save_at = now + 1000000000ULL;   /* 1s after the last */
                printf("sj: stick sensitivity %d%%\n", v);
            }
        }
        if (g_sens_save_at && now >= g_sens_save_at) {
            g_sens_save_at = 0;
            write_config(g_cfg_path);
        }
    }

    /* Clicking the left stick turns the controller's motion off and back on.
     *
     * Tilt is the faithful control but it is not always wanted -- lying down,
     * on a bus, or simply preferring the stick -- and it is the sort of choice
     * you make mid-level, not in a menu. The stick click is otherwise unused
     * and is right under the thumb that is already steering.
     *
     * Saved, because someone who wants stick-only wants it next session too. */
    {
        static int stick_prev;
        const int down = (padGetButtonsDown(&g_pad) & HidNpadButton_StickL) != 0;
        if (down && !stick_prev) {
            config.gyro      = !config.gyro;
            config.tilt_mode = config.gyro ? TILT_BOTH : TILT_STICK;
            if (config.gyro) sixaxis_init();       /* first time on: start them */
            sj_sensor_set_mode(config.tilt_mode);   /* also re-neutralises */
            write_config(g_cfg_path);
            printf("sj: gyro %s (left stick click)\n",
                   config.gyro ? "ON -- stick and motion" : "OFF -- stick only");
        }
        stick_prev = down;
    }

    {
        static int combo_prev;
        const u64 held = padGetButtons(&g_pad);
        const int combo = (held & TILT_INVERT_COMBO) == TILT_INVERT_COMBO;

        if (combo) {
            if (!combo_prev) {
                config.tilt_invert = !config.tilt_invert;
                write_config(g_cfg_path);          /* survives a restart */
                printf("sj: tilt invert %s (L+R+ZL+ZR)\n",
                       config.tilt_invert ? "ON" : "off");
            }
            combo_prev = 1;

            /* Drop this frame's pointer events and release anything held. */
            n = 0;
            if (g_ptr_owner >= 0 && !g_ptr_up_pending) {
                g_ptr_up_pending = 1;
                g_ptr_up_x = (float)screen_width  * 0.5f;
                g_ptr_up_y = (float)screen_height * 0.5f;
            }
        } else {
            combo_prev = 0;
        }
    }

    /* A / X / Y (and B, when B is not Back) tap the centre of the screen.
     *
     * The mid-air jump is a tap anywhere, and aiming a cursor to do it is
     * miserable. These all poke the middle, which is what a phone player's
     * thumb does, and
     * they are deliberately separate from ZL/ZR, which tap wherever the cursor
     * is for menus. Held down, the tap holds -- the game distinguishes a tap
     * from a hold. */
    {
        const u64 held = padGetButtons(&g_pad);
        const int want = (held & JUMP_BUTTONS) != 0;

        if (want && g_ptr_owner < 0) {
            float cx = (float)screen_width * 0.5f;
            float cy = (float)screen_height * 0.5f;
            static int logged;
            if (logged < 3) {
                printf("sj: jump button -> tap at centre %.0f,%.0f\n", cx, cy);
                logged++;
            }
            g_ptr_owner      = JUMP_PTR_ID;
            g_ptr_down_ns    = now;
            g_ptr_move_ns    = now;
            g_ptr_up_pending = 0;
            g_touch_down     = 1;
            sj_n_touches++;
            deliver(SJ_MOTION_DOWN, cx, cy);
        } else if (!want && g_ptr_owner == JUMP_PTR_ID && !g_ptr_up_pending) {
            g_ptr_up_pending = 1;
            g_ptr_up_x = (float)screen_width  * 0.5f;
            g_ptr_up_y = (float)screen_height * 0.5f;
        }
    }

    for (i = 0; i < n; i++) {
        const int id = ev[i].id;

        if (ev[i].phase == NXP_DOWN) {
            if (g_ptr_owner >= 0) continue;         /* already tracking one */
            g_ptr_owner     = id;
            g_ptr_down_ns   = now;
            g_ptr_move_ns   = now;
            g_ptr_up_pending = 0;
            g_touch_down    = 1;
            sj_n_touches++;
            {
                static int logged;
                if (logged < 6) {
                    printf("sj: touch DOWN id=%d at %.0f,%.0f\n",
                           id, ev[i].x, ev[i].y);
                    logged++;
                }
            }
            deliver(SJ_MOTION_DOWN, ev[i].x, ev[i].y);
            continue;
        }

        if (id != g_ptr_owner) continue;            /* not the pointer we track */

        if (ev[i].phase == NXP_UP) {
            /* Hold the release until the press has lasted long enough to
             * survive the engine's batched processing. */
            g_ptr_up_pending = 1;
            g_ptr_up_x = ev[i].x;
            g_ptr_up_y = ev[i].y;
            continue;
        }

        if (now - g_ptr_move_ns >= TOUCH_MOVE_MIN_NS) {
            g_ptr_move_ns = now;
            deliver(SJ_MOTION_MOVE, ev[i].x, ev[i].y);
        }
    }

    if (g_ptr_up_pending && now - g_ptr_down_ns >= TOUCH_MIN_HOLD_NS) {
        static int logged;
        if (logged < 6) {
            printf("sj: touch UP   id=%d at %.0f,%.0f (held %llu ms)\n",
                   g_ptr_owner, g_ptr_up_x, g_ptr_up_y,
                   (unsigned long long)((now - g_ptr_down_ns) / 1000000ULL));
            logged++;
        }
        deliver(SJ_MOTION_UP, g_ptr_up_x, g_ptr_up_y);
        g_ptr_up_pending = 0;
        g_ptr_owner      = -1;
        g_touch_down     = 0;
    }

    /* B -> the game's Back handler.
     *
     * Fever's input handler declines every key event; the Java activity
     * forwarded onBackPressed through Loader.triggerBack() instead, so that is
     * what B calls. sj_jni_send_back declines whenever Back would reach the
     * home screen, where it quits the game outright with no confirmation --
     * which includes most of a level, since the in-game screen passes Back
     * through. With back_button=0, B is a jump button like A/X/Y. */
    if (config.back_button && (padGetButtonsDown(&g_pad) & HidNpadButton_B)) {
        if (sj_jni_send_back()) printf("sj: B -> Back\n");
    }
}

/* --- library extraction ----------------------------------------------------
 * The user copies their APK; the library is pulled out of it here. It is
 * re-extracted whenever the copy on the card does not match the APK's entry in
 * size, which covers both a first boot and an APK that was swapped for a
 * different build. */
static void ensure_library(void)
{
    char lib[600];
    const char *apk = sj_apk();
    SjZipEntry e;
    struct stat st;
    int have = 0;

    sj_path(lib, sizeof(lib), LIB_MAIN);
    have = stat(lib, &st) == 0 && S_ISREG(st.st_mode);

    if (!apk || !*apk) {
        if (have) return;           /* no APK, but a library: let it try */
        fatal_error("No Sonic Jump Fever APK found in\n%s\n\nCopy your own "
                    "APK (the arm64-v8a build) into this folder next to\n"
                    "sonicjumpfever_nx.nro. Any filename works.", sj_home());
    }
    if (sj_zip_find(apk, LIB_APK_PATH, &e) != 0)
        fatal_error("%s\n\ndoes not contain %s.\n\nYou need the arm64 build of "
                    "Sonic Jump Fever: a split APK pulled from a\n32-bit phone "
                    "only has armeabi-v7a and cannot run here.", apk, LIB_APK_PATH);

    if (have && (uint64_t)st.st_size == e.size) return;

    startup_status_update("First boot: extracting the game library...");
    printf("sj: extracting %s from %s (%u bytes)\n", LIB_APK_PATH, apk, e.size);
    if (sj_zip_extract(apk, LIB_APK_PATH, lib) != 0)
        fatal_error("Could not extract %s from\n%s\n\nThe APK may be damaged, "
                    "or the SD card full or read-only.", LIB_APK_PATH, apk);
    printf("sj: wrote %s\n", lib);
}

/* --- module loading -------------------------------------------------------- */

static int load_game(void)
{
    static void *base;
    /* Source arena for the module image. libsonicjumpfever.so maps to
     * ~6.1 MB (its last LOAD segment ends at 0x60a588); 16 MB is ample
     * headroom without stranding memory the engine wants for textures. It
     * cannot simply be freed later either: so_finalize donates the first
     * load_size bytes to the kernel as code memory. so_load bounds-checks
     * against this, so a larger module fails cleanly. */
    const size_t reserve = 16u * 1024 * 1024;

    base = memalign(0x1000, reserve);
    if (!base) fatal_error("Out of memory reserving the %u MB module arena.",
                           (unsigned)(reserve / (1024 * 1024)));

    {
        char lib[600];
        sj_path(lib, sizeof(lib), LIB_MAIN);
        int rc = so_load(&main_mod, lib, base, reserve);
        if (rc < 0)
            fatal_error("Could not load %s (error %d).\n\nDelete it and start "
                        "again: it is re-extracted from your APK.", lib, rc);
    }

    if (so_relocate(&main_mod) < 0)
        fatal_error("Relocation of %s failed.", LIB_MAIN);

    /* libsonicjumpfever.so is BIND_NOW: every import must bind before the
     * first call, so a missing symbol is a hard error here rather than a
     * mystery crash later. resolve_imports() taints anything it cannot find
     * and so_resolve reports it. */
    resolve_imports(&main_mod);

    /* debug_log=1: log the first call the game makes to each import, before
     * the call runs (sj_calltrace.c). Must follow resolve_imports, which
     * fills the slots it redirects, and precede so_finalize, which maps them
     * where the game runs them. */
    if (config.debug_log) sj_calltrace_install(&main_mod);

    /* ORDER MATTERS, and getting it wrong is a null-ish Data Abort at boot.
     *
     * so_load only *reserves* the virtual range (virtmemFindCodeMemory +
     * virtmemAddReservation). A reservation is bookkeeping -- nothing is
     * mapped there yet. so_finalize performs the actual
     * svcMapProcessCodeMemory that aliases load_base to load_virtbase and sets
     * page permissions.
     *
     * so_flush_caches touches load_virtbase directly, so it MUST come after
     * so_finalize. Flushing first faults on the very first cache line, which
     * shows up as a Data Abort inside armDCacheFlush with no obvious
     * connection to the loader. All three reference ports do finalize-then-
     * flush; an earlier version of this file had them the other way round. */
    so_finalize(&main_mod);
    so_flush_caches(&main_mod);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    startup_status_begin("Preparing Sonic Jump Fever");

    if (sj_paths_init(argc, argv) != 0)
        fatal_error("Could not find Sonic Jump Fever.\n\nCopy your own Sonic "
                    "Jump Fever APK (arm64-v8a) into\n%s\nnext to this .nro. "
                    "Any filename works.", sj_home());
    /* Logging first: everything below can fail, and a printf without this is
     * a crash rather than a message (libnx routes it to a console we never
     * created). sj_log_init also redirects stdout/stderr, so the plain
     * printf() calls throughout this port and the inherited shims are safe
     * from here on. */
    sj_log_init();
    printf("sj: game folder %s (%s)\n", sj_home(), sj_paths_origin());
    printf("sj: log at %s\n", sj_log_path());

    /* Before any thread exists: records the allowed cores and the main
     * thread's real priority, which the engine-thread spreading and the
     * watchdog are both placed relative to. */
    sj_threads_init();

    if (chdir(sj_home()) != 0)
        fatal_error("Could not open %s.", sj_home());

    sj_path(g_cfg_path, sizeof(g_cfg_path), CONFIG_NAME);
    if (read_config(g_cfg_path) != 0) write_config(g_cfg_path);

    /* Apply the logging setting as soon as it is known. The log has to start
     * before this -- everything above it can fail and would otherwise fail
     * silently -- so switching it off happens here rather than at init. */
    if (!config.debug_log) sj_log_set_enabled(0);

    /* save_edit.txt -> profile0.dat, before the game reads its profile. The
     * first boot only writes the template (every line commented out). */
    sj_saveedit_run();

    /* Print what the config actually resolved to. "I set rotation=1 and nothing
     * happened" is otherwise impossible to tell apart from "the file was never
     * re-read", "the value did not parse" and "the rotation call failed". */
    printf("sj: config: language=%s rotation=%d (0=off 1=90cw 2=90ccw) "
           "gyro=%d watchdog=%d\n", config.language, config.rotation,
           config.gyro, config.watchdog);

    /* The engine reads every asset out of the APK itself ("apk:assets/..."),
     * and the library comes out of it too, so the APK is the one thing that
     * must be here. */
    printf("sj: APK: %s\n", sj_apk()[0] ? sj_apk() : "(none found)");
    ensure_library();

    if (appletGetOperationMode() == AppletOperationMode_Console) {
        screen_width = 1920; screen_height = 1080;
    } else {
        screen_width = 1280; screen_height = 720;
    }

    /* Portrait (TATE) output.
     *
     * `rotation` used to only compensate the pointer and the accelerometer --
     * it assumed something else had already rotated the picture, which nothing
     * had. To actually display in portrait the window itself has to be turned:
     * give it portrait dimensions and a rotation transform, and the compositor
     * rotates the finished frame on the way to the panel.
     *
     * This is worth more than a cosmetic change. Fever is a portrait game;
     * with a portrait surface eglQuerySurface reports 720x1280 and the engine
     * lays out at its native aspect instead of being squashed into a landscape
     * frame.
     *
     * Transform values follow Android's HAL constants, which is what libnx's
     * nwindow layer passes through: ROT_90 = 4, ROT_180 = 3, ROT_270 = 7. */
    /* Portrait (TATE) output.
     *
     * Only the logical size is swapped here. The window itself is configured
     * in the eglCreateWindowSurface wrapper in imports.c -- setting dimensions
     * at startup fails, and poinpy shows why: the call belongs immediately
     * before the surface is created. Everything downstream (viewport, pointer,
     * touch mapping) works from screen_width/height, so they have to be right
     * from here on regardless. */
    if (config.rotation) {
        int t2 = screen_width; screen_width = screen_height; screen_height = t2;
        printf("sj: portrait requested: logical screen %dx%d (window is set up "
               "when the game creates its surface)\n",
               screen_width, screen_height);
    } else {
        printf("sj: landscape output (set rotation=1 in config.txt for "
               "portrait)\n");
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    /* gyro=0 leaves the motion sensors switched off entirely -- not started,
     * not polled -- until the stick click turns motion on. Nothing else in
     * the port needs them, so stick-only play never touches the sensor path. */
    if (config.gyro) sixaxis_init();
    else printf("sj: gyro=0 -- motion sensors not started (stick only)\n");

    /* Pointer module. Init early: it reads cursor.png off the SD card before
     * the engine spawns its threads, and decodes/uploads lazily on the render
     * thread. handle_touch=1 hands it the touchscreen as well as the stick. */
    {
        NxpConfig np;
        memset(&np, 0, sizeof(np));
        np.screen_w     = screen_width;
        np.screen_h     = screen_height;
        /* The touch panel is physically 1280x720 whatever the window does. */
        np.panel_w      = 1280;
        np.panel_h      = 720;
        np.data_dir     = sj_home();
        np.handle_touch = 1;
        /* This is exactly what nx_pointer's rotation field is for: the
         * compositor rotates the framebuffer, the cursor stays in RENDER
         * space, and physical device input (touch panel, stick, mouse) is
         * rotated to match. Passing 0 last time -- on the reasoning that the
         * window was already rotated so nothing more was needed -- left the
         * touch panel reporting landscape coordinates into a portrait render
         * space, which is the offset. */
        np.rotation     = config.rotation;
        np.stick_speed  = 0.0f;          /* module defaults */
        np.mouse_sens   = 0.0f;
        nxp_init(&np);
        printf("sj: pointer ready (%dx%d render, rotation=%d)\n",
               np.screen_w, np.screen_h, np.rotation);
    }
    sj_sensor_set_mode(config.tilt_mode);

    startup_status_update("Loading the game module...");
    load_game();

    static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
    install_bionic_tls(main_tls);

    startup_status_update("Starting the Android compatibility layer...");
    jni_init();
    android_native_init();
    sj_input_init();
    /* Open the audio device now rather than leaving it to the first
     * CreateAudioPlayer. Music goes through the same mixer, so if the game
     * played a track before creating any sound effect the output would be
     * silent -- and the device-open result is the single most useful line in
     * the log when there is no sound. */
    opensles_ensure_output();
    sj_music_init(sj_assets());   /* mixes into opensles, no own device */
    /* OpenSL initialises lazily on the engine's first slCreateEngine call. */

    so_execute_init_array(&main_mod);   /* EASTL + tinyxml2 static ctors */

    /* Hand the fake JavaVM to the module.
     *
     * On Android this is called by the runtime when System.loadLibrary maps
     * the .so. Nothing does it here, so we must. It is not optional: the
     * module keeps the vm in its own `g_JVM` global and android_main's very
     * first act is g_JVM->GetEnv(...). With g_JVM still NULL that is a load
     * from address 0 -- a Data Abort inside android_main + 0x68, on the glue
     * thread, several layers away from anything that looks like our code.
     *
     * After the INIT_ARRAY, because JNI_OnLoad calls into machinery those
     * static constructors set up. Before onCreate, because the glue thread it
     * spawns runs android_main immediately.
     *
     * All three reference wrappers do this too. */
    {
        typedef int (*jni_onload_fn)(void *vm, void *reserved);
        jni_onload_fn on_load =
            (jni_onload_fn)so_try_find_addr_rx(&main_mod, "JNI_OnLoad");
        if (on_load) {
            int ver = on_load(fake_vm, NULL);
            printf("sj: JNI_OnLoad -> 0x%08x\n", ver);
            /* JNI_OnLoad returns the JNI version it wants, or JNI_ERR (-1).
             * It only reaches the g_JVM store if GetEnv succeeded, so a
             * negative return means the vm was rejected and g_JVM is still
             * NULL -- android_main would fault exactly as before. Say so now
             * rather than letting it crash somewhere unrecognisable. */
            if (ver < 0)
                fatal_error("JNI_OnLoad rejected the JavaVM (returned %d).\n\n"
                            "The module could not get a JNIEnv, so its g_JVM "
                            "is still NULL and android_main will fault.", ver);
        } else {
            printf("sj: no JNI_OnLoad export; skipping\n");
        }
    }

    /* The temporary file image (~4.4 MB) is only needed up to this point:
     * relocation, resolution and the static constructors all read from it.
     * Freeing it here mirrors the reference ports and hands the memory back
     * before the engine starts allocating textures. */
    so_free_temp(&main_mod);

    sj_jni_resolve_callbacks();

    /* --- hand control to the engine ------------------------------------- */
    ANativeActivity_createFunc *onCreate =
        (ANativeActivity_createFunc *)so_find_addr_rx(&main_mod, "ANativeActivity_onCreate");
    if (!onCreate) fatal_error("ANativeActivity_onCreate not found in %s.", LIB_MAIN);

    ANativeActivity *activity = android_make_activity(
        fake_vm, fake_env, jni_make_activity_object(),
        NULL,                          /* no AAssetManager: the engine fopen()s */
        sj_home_slash(), sj_assets(), sj_home_slash());

    startup_status_end();

    /* Arm the watchdog before handing over, so a hang anywhere from here on
     * -- the lifecycle handshake included -- ends in a crash report naming
     * every thread instead of a frozen console. The 3 s status lines are
     * only printed with debug_log=1; the watchdog runs either way. */
    if (config.watchdog) sj_trace_start(config.debug_log);
    sj_wd_main_beat();

    printf("sj: calling ANativeActivity_onCreate\n");
    onCreate(activity, NULL, 0);
    printf("sj: onCreate returned (glue thread is running)\n");

    /* The standard lifecycle, in Android's order. Each of these is marshalled
     * to the glue thread and blocks until it acknowledges, so the log below
     * shows exactly how far the handshake got if one of them hangs. */
    printf("sj: lifecycle onStart\n");
    if (activity->callbacks->onStart)  activity->callbacks->onStart(activity);
    printf("sj: lifecycle onResume\n");
    if (activity->callbacks->onResume) activity->callbacks->onResume(activity);

    printf("sj: lifecycle onNativeWindowCreated\n");
    if (activity->callbacks->onNativeWindowCreated)
        activity->callbacks->onNativeWindowCreated(activity, android_native_window());
    /* Releases the looper gate: until this point ALooper_pollOnce refuses to
     * report an empty queue, so the glue cannot reach appUpdate before the
     * game has built its ScreenManager. */
    android_native_window_delivered();

    printf("sj: lifecycle onInputQueueCreated\n");
    if (activity->callbacks->onInputQueueCreated)
        activity->callbacks->onInputQueueCreated(activity, android_input_queue());
    printf("sj: lifecycle onWindowFocusChanged\n");
    if (activity->callbacks->onWindowFocusChanged)
        activity->callbacks->onWindowFocusChanged(activity, 1);
    printf("sj: lifecycle delivered; entering the main loop\n");

    sj_jni_push_initial_state();

    int focused = (appletGetFocusState() == AppletFocusState_InFocus);
    printf("sj: initial focus state = %d (InFocus=%d)\n",
           appletGetFocusState(), AppletFocusState_InFocus);
    sj_wd_set_focus(focused);
    while (appletMainLoop()) {
        sj_wd_main_beat();
        sj_mark("main loop");
        padUpdate(&g_pad);

        if (sj_finish_requested()) break;
        /* Minus used to quit outright. With no confirmation and no on-screen
         * feedback that is indistinguishable from a crash, so it is off unless
         * quit_button=1 is set. Use the HOME button to leave. */
        if (config.quit_button &&
            (padGetButtonsDown(&g_pad) & HidNpadButton_Minus)) {
            printf("sj: Minus pressed -- quitting (quit_button=1)\n");
            break;
        }

        /* Applet focus: pause the engine when the player opens the home menu,
         * or audio keeps playing over it and the save state drifts. */
        int now_focused = appletGetFocusState() == AppletFocusState_InFocus;
        if (now_focused != focused) {
            /* Worth logging: if the console ever reports us out of focus at
             * startup we would pause the game immediately, which is
             * indistinguishable from a hang -- the logo stays up and nothing
             * moves. */
            printf("sj: focus %d -> %d (appletGetFocusState=%d)\n",
                   focused, now_focused, appletGetFocusState());
            focused = now_focused;
            sj_wd_set_focus(focused);
            if (activity->callbacks->onWindowFocusChanged)
                activity->callbacks->onWindowFocusChanged(activity, focused);
            if (!focused) {
                /* Release any held press before pausing, then drop the queue.
                 * Otherwise a finger down at the moment the home menu opens
                 * leaves the engine with a touch that never ends. */
                if (g_touch_down) {
                    /* Release the held press before pausing, or the engine
                     * keeps a touch active across the home menu. */
                    float x = 0.0f, y = 0.0f;
                    nxp_cursor_pos(&x, &y);
                    deliver(SJ_MOTION_UP, x, y);
                    g_touch_down     = 0;
                    g_ptr_owner      = -1;
                    g_ptr_up_pending = 0;
                }
                sj_input_reset();
            }
            if (!focused && activity->callbacks->onPause)  activity->callbacks->onPause(activity);
            if (focused  && activity->callbacks->onResume) activity->callbacks->onResume(activity);
        }
        if (!focused) { svcSleepThread(16000000ULL); continue; }

        /* Deliver any billing/network replies queued from inside a JNI
         * handler. They must land here, outside the engine's store lock. */
        sj_jni_pump_deferred();

        pump_touch();
        sj_sensor_update();

        /* The engine's own ALooper_pollOnce drains the queue and renders; we
         * just need to yield so its thread runs. See android_native.c for how
         * pollOnce reports readiness. */
        android_native_frame();
    }

    /* Leaving: rendering stops and the teardown below blocks on the glue
     * thread by design, neither of which is a hang. */
    sj_wd_disarm();

    /* Ordered teardown. Skipping it loses the player's progress: the engine
     * writes its save from onPause. The order mirrors Android's own teardown
     * -- focus, pause, surface gone, input gone, stop, destroy -- because the
     * glue marshals each of these to its thread and asserts on the sequence.
     *
     * Unless the quit came through exit(), in which case the calling thread is
     * parked inside sj_exit_park() and every one of these callbacks would block
     * forever waiting for it to acknowledge. */
    if (sj_quit_via_exit()) {
        printf("sj: quit came from exit(); skipping the activity lifecycle\n");
    } else {
        if (activity->callbacks->onWindowFocusChanged)
            activity->callbacks->onWindowFocusChanged(activity, 0);
        if (activity->callbacks->onPause)   activity->callbacks->onPause(activity);
        if (activity->callbacks->onNativeWindowDestroyed)
            activity->callbacks->onNativeWindowDestroyed(activity, android_native_window());
        if (activity->callbacks->onInputQueueDestroyed)
            activity->callbacks->onInputQueueDestroyed(activity, android_input_queue());
        if (activity->callbacks->onStop)    activity->callbacks->onStop(activity);
        if (activity->callbacks->onDestroy) activity->callbacks->onDestroy(activity);
    }

    /* Shut down only what we own, then leave the process immediately.
     *
     * The engine keeps its own threads -- the crash report from a Back-to-quit
     * showed an Instruction Abort with the faulting thread still executing
     * inside the game library at its audio thread's entry, well after
     * onDestroy had returned. Nothing in the NativeActivity contract lets us
     * join those threads, and they do not stop when the glue thread does.
     *
     * So the ordering that matters is: stop our own worker and detach our
     * callbacks first, so an engine thread cannot re-enter a half-torn-down
     * mixer; flush the log while it is still meaningful; then exit the process
     * outright rather than returning through libnx's teardown, which unmaps
     * the module out from under any engine thread still running in it.
     *
     * opensles_shutdown() is deliberately NOT called: the engine's audio
     * thread may still be inside it, and the process exit reclaims the device
     * anyway. */
    sj_music_exit();                      /* our decode thread; we can join it */
    opensles_set_music_source(NULL, NULL);/* mixer stops calling into us */
    for (int i = 0; i < 4; i++) hidStopSixAxisSensor(g_sixaxis[i]);
    sj_trace_stop();
    printf("sj: shutdown complete; exiting\n");
    sj_log_exit();

    /* Terminating every thread at once. Returning from main() lets libnx run
     * its teardown and unmap the module while an engine thread may still be
     * executing inside it -- the Instruction Abort seen on quit had the
     * faulting thread sitting at the engine's audio-thread entry.
     *
     * If a quit ever looks like a crash rather than a clean return to hbmenu,
     * set clean_exit=1 in config.txt to return normally instead and send the
     * new crash report: the two paths fail in different places and the report
     * distinguishes them. */
    if (config.clean_exit) return 0;
    svcExitProcess();
    return 0;   /* not reached */
}

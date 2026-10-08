/* android_native.c -- ANativeWindow / ALooper / AConfiguration for a plain
 * NativeActivity title.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 * Derived from angrybirdsjourney_nx's android_native_unity.c,
 * (C) 2021 Andy Nguyen, fgsfds, with the Unity-specific paths removed and the
 * portrait handling rewritten.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <switch.h>

#include "android_native.h"
#include "sj_ndk.h"
#include "sj_input.h"
#include "sj_trace.h"
#include "sj_sensor.h"
#include "sj_input.h"
#include "sj_trace.h"
#include "sj_sensor.h"
#include "sj_sensor.h"
#include "config.h"
#include "fakefd.h"

#define AWINDOW_FORMAT_RGBA_8888 1

/* android_native_app_glue's idents. */
#define LOOPER_ID_MAIN   1
#define LOOPER_ID_INPUT  2

static u32 g_win_w = 1280, g_win_h = 720;   /* real framebuffer            */
static u32 g_game_w = 640, g_game_h = 1136; /* only meaningful once the engine says so */

/* Set once the engine actually asks for a surface size. Until then it takes its
 * dimensions from eglQuerySurface -- the real framebuffer -- and any transform
 * we apply to touch is pure corruption.
 *
 * Sonic Jump never calls setBuffersGeometry and never calls
 * ANativeWindow_getWidth: it renders straight into the 1280x720 surface. The
 * assumed 640x1136 portrait geometry mapped a tap at physical 786,570 to
 * 550,899 -- a y beyond the bottom of a 720-pixel screen. */
static int g_geometry_from_engine;
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;

void android_native_update_mode(void)
{
    /* screen_width/height already account for a portrait window: main() swaps
     * them when it rotates the output. Follow them rather than re-deriving
     * from the operation mode, which knows nothing about the transform. */
    g_win_w = (u32)screen_width;
    g_win_h = (u32)screen_height;
}

u32 android_native_width(void)  { return g_win_w; }
u32 android_native_height(void) { return g_win_h; }

void android_native_init(void)
{
    android_native_update_mode();
    sj_input_init();
    printf("sj: window %ux%u; touch is 1:1 unless the engine calls "
           "setBuffersGeometry\n", g_win_w, g_win_h);
}

/* --- window ---------------------------------------------------------------
 * Sonic Jump is portrait. We keep the real EGL surface landscape (the panel is
 * landscape and rotating the swapchain costs a full-screen blit every frame)
 * and instead let the engine believe it has a portrait window, pillarboxed in
 * the centre. The engine is safe-area aware, so its HUD adapts.
 * ------------------------------------------------------------------------ */

ANativeWindow *android_native_window(void)
{
    return (ANativeWindow *)nwindowGetDefault();
}

int32_t ANativeWindow_getWidth(ANativeWindow *w)
{
    static int once;
    (void)w;
    if (!once) { once = 1; printf("sj: ANativeWindow_getWidth -> %u\n", g_game_w); }
    return (int32_t)g_game_w;
}
int32_t ANativeWindow_getHeight(ANativeWindow *w)
{
    static int once;
    (void)w;
    if (!once) { once = 1; printf("sj: ANativeWindow_getHeight -> %u\n", g_game_h); }
    return (int32_t)g_game_h;
}
int32_t ANativeWindow_getFormat(ANativeWindow *w) { (void)w; return AWINDOW_FORMAT_RGBA_8888; }
void    ANativeWindow_acquire(ANativeWindow *w)   { (void)w; }
void    ANativeWindow_release(ANativeWindow *w)   { (void)w; }

/* Sonic Jump Fever calls this exactly once, from its EGL init, as
 *     ANativeWindow_setBuffersGeometry(window, 0, 0, EGL_NATIVE_VISUAL_ID)
 * -- the NDK sample idiom, where 0x0 means "keep the window's own size". So
 * it never actually asks for a geometry, takes its size from eglQuerySurface,
 * and touch stays 1:1. A non-zero request would be recorded and the touch
 * mapping pillarboxed to match; the real swapchain is never resized. */
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width,
                                         int32_t height, int32_t format)
{
    (void)w; (void)format;
    if (width > 0 && height > 0) {
        g_game_w = (u32)width;
        g_game_h = (u32)height;
        g_geometry_from_engine = 1;
        printf("sj: engine requested %dx%d, presenting pillarboxed in %ux%u\n",
               width, height, g_win_w, g_win_h);
    }
    return 0;
}

/* The portrait image is centred in the landscape framebuffer and scaled to
 * fill its height. Both the touch mapping and the on-screen rectangle derive
 * from this one function: computing them separately (floats in one place,
 * truncated ints in the other) put the drawn rectangle and the input mapping
 * a fraction of a pixel apart, which is small but is exactly the kind of
 * discrepancy that turns into "taps land just off the button". */
/* Set once the engine actually asks for a surface size. Until then it is
 * taking its dimensions from eglQuerySurface -- i.e. the real framebuffer --
 * and any transform we apply to touch is pure corruption.
 *
 * Sonic Jump never calls setBuffersGeometry and never calls
 * ANativeWindow_getWidth: it renders straight into the 1280x720 surface. The
 * assumed 640x1136 portrait geometry was mapping a tap at physical 786,570 to
 * 550,899 -- a y beyond the bottom of a 720-pixel screen. */
static void viewport_f(float *out_ox, float *out_scale, float *out_draw_w)
{
    float scale, draw_w;

    if (!g_geometry_from_engine) {          /* 1:1, the common case here */
        if (out_scale)  *out_scale  = 1.0f;
        if (out_draw_w) *out_draw_w = (float)g_win_w;
        if (out_ox)     *out_ox     = 0.0f;
        return;
    }
    scale  = (float)g_win_h / (float)g_game_h;
    draw_w = (float)g_game_w * scale;
    if (out_scale)  *out_scale  = scale;
    if (out_draw_w) *out_draw_w = draw_w;
    if (out_ox)     *out_ox     = ((float)g_win_w - draw_w) * 0.5f;
}

/* Map a physical touch/cursor point into the engine's portrait coordinate
 * space. Points in the letterbox bars clamp to the nearest edge rather than
 * going negative -- the engine treats a negative touch as a separate gesture. */
void android_window_to_game(float *x, float *y)
{
    float ox, scale;
    float gx, gy;

    viewport_f(&ox, &scale, NULL);
    if (scale <= 0.0f) return;

    gx = (*x - ox) / scale;
    gy = *y / scale;

    {
        float mx = g_geometry_from_engine ? (float)g_game_w : (float)g_win_w;
        float my = g_geometry_from_engine ? (float)g_game_h : (float)g_win_h;
        if (gx < 0.0f) gx = 0.0f;
        if (gy < 0.0f) gy = 0.0f;
        if (gx > mx) gx = mx;
        if (gy > my) gy = my;
    }

    *x = gx;
    *y = gy;
}

/* Where the portrait image lands inside the landscape framebuffer. Rounded,
 * not truncated, so it agrees with android_window_to_game to within half a
 * pixel instead of drifting toward one side. */
void android_game_viewport(int *x, int *y, int *w, int *h)
{
    float ox, draw_w;

    viewport_f(&ox, NULL, &draw_w);
    if (x) *x = (int)(ox + 0.5f);
    if (y) *y = 0;
    if (w) *w = (int)(draw_w + 0.5f);
    if (h) *h = (int)g_win_h;
}

/* --- activity -------------------------------------------------------------- */

ANativeActivity *android_make_activity(void *vm, void *env, void *clazz,
                                       AAssetManager *am,
                                       const char *internalPath,
                                       const char *externalPath,
                                       const char *obbPath)
{
    memset(&g_activity, 0, sizeof(g_activity));
    memset(&g_callbacks, 0, sizeof(g_callbacks));
    g_activity.callbacks        = &g_callbacks;
    g_activity.vm               = vm;
    g_activity.env              = env;
    g_activity.clazz            = clazz;
    g_activity.internalDataPath = internalPath;
    g_activity.externalDataPath = externalPath;
    g_activity.obbPath          = obbPath;
    g_activity.assetManager     = am;
    /* API 23. The game's own build targets min-api 23, and claiming a much
     * newer level makes it probe for behaviour we do not emulate. */
    g_activity.sdkVersion       = 23;
    return &g_activity;
}

/* --- looper ----------------------------------------------------------------
 * Sonic Jump links the stock android_native_app_glue (the binary contains
 * android_app_read_cmd / android_app_pre_exec_cmd / app_dummy / threaded_app).
 * That means:
 *
 *   - ANativeActivity_onCreate spawns the glue's own thread, which runs the
 *     game loop. It returns as soon as that thread is up.
 *   - Our thread calls activity->callbacks->onStart/onResume/... The glue's
 *     implementations of those do not run game code; they write a one-byte
 *     APP_CMD_* down a pipe and, for the synchronous ones, wait for the glue
 *     thread to acknowledge.
 *   - The glue thread sits in ALooper_pollOnce, blocking, waiting for that
 *     pipe (registered via ALooper_addFd with ident LOOPER_ID_MAIN) or for
 *     input (ident LOOPER_ID_INPUT via AInputQueue_attachLooper).
 *
 * So pollOnce must really watch the registered fd and really block. An earlier
 * version of this file ignored the fd and always returned TIMEOUT; the glue
 * would then never read APP_CMD_INIT_WINDOW, never create its EGL surface, and
 * the game would sit at a black screen for ever while looking perfectly alive.
 * ------------------------------------------------------------------------ */

#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_CALLBACK (-2)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define ALOOPER_POLL_ERROR    (-4)

#define ALOOPER_EVENT_INPUT   0x01

#define MAX_LOOPER_FDS 8

typedef struct {
  int   fd;
  int   ident;
  int   events;
  ALooper_callbackFunc callback;
  void *data;
  int   used;
} LooperFd;

static LooperFd g_lfds[MAX_LOOPER_FDS];

/* Set once main() has delivered onNativeWindowCreated. Until then a zero
 * timeout must not report "nothing to do" -- see ALooper_pollOnce. */
static volatile int g_window_delivered;
static uint64_t     g_first_poll_ns;

void android_native_window_delivered(void) { g_window_delivered = 1; }
static Mutex    g_looper_lock;
static int      g_looper_ready;
static int      g_looper_token;
static int      g_wake_flag;

static void looper_once(void) {
  if (!g_looper_ready) { mutexInit(&g_looper_lock); g_looper_ready = 1; }
}

ALooper *ALooper_prepare(int opts) {
  (void)opts;
  looper_once();
  return (ALooper *)&g_looper_token;
}

ALooper *ALooper_forThread(void) { looper_once(); return (ALooper *)&g_looper_token; }
void ALooper_acquire(ALooper *l) { (void)l; }
void ALooper_release(ALooper *l) { (void)l; }

void ALooper_wake(ALooper *l) {
  (void)l;
  looper_once();
  mutexLock(&g_looper_lock);
  g_wake_flag = 1;
  mutexUnlock(&g_looper_lock);
}

int ALooper_addFd(ALooper *l, int fd, int ident, int events,
                  ALooper_callbackFunc callback, void *data) {
  int i;
  (void)l;
  looper_once();
  mutexLock(&g_looper_lock);
  for (i = 0; i < MAX_LOOPER_FDS; i++) {
    if (g_lfds[i].used && g_lfds[i].fd == fd) break;      /* replace */
    if (!g_lfds[i].used) break;
  }
  if (i == MAX_LOOPER_FDS) { mutexUnlock(&g_looper_lock); return -1; }
  g_lfds[i].fd = fd;
  /* ALOOPER_POLL_CALLBACK (== -2) as ident means "use the callback"; the glue
   * passes a real ident for its command pipe. */
  g_lfds[i].ident    = (callback && ident <= 0) ? ALOOPER_POLL_CALLBACK : ident;
  g_lfds[i].events   = events ? events : ALOOPER_EVENT_INPUT;
  g_lfds[i].callback = callback;
  g_lfds[i].data     = data;
  g_lfds[i].used     = 1;
  mutexUnlock(&g_looper_lock);
  return 1;
}

int ALooper_removeFd(ALooper *l, int fd) {
  int i;
  (void)l;
  looper_once();
  mutexLock(&g_looper_lock);
  for (i = 0; i < MAX_LOOPER_FDS; i++)
    if (g_lfds[i].used && g_lfds[i].fd == fd) g_lfds[i].used = 0;
  mutexUnlock(&g_looper_lock);
  return 1;
}

/* Scan the registered fds once. Returns the ident of the first ready source,
 * or 0 if nothing is ready. */
static int poll_scan(int *outFd, int *outEvents, void **outData) {
  int i, ident = 0;
  mutexLock(&g_looper_lock);
  for (i = 0; i < MAX_LOOPER_FDS; i++) {
    if (!g_lfds[i].used) continue;
    if (!fakefd_readable(g_lfds[i].fd)) continue;
    if (outFd)     *outFd     = g_lfds[i].fd;
    if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
    if (outData)   *outData   = g_lfds[i].data;
    if (g_lfds[i].ident == ALOOPER_POLL_CALLBACK && g_lfds[i].callback) {
      ALooper_callbackFunc cb = g_lfds[i].callback;
      void *d = g_lfds[i].data;
      int fd = g_lfds[i].fd;
      mutexUnlock(&g_looper_lock);
      /* The callback drains the fd itself; a 0 return means "unregister". */
      if (cb(fd, ALOOPER_EVENT_INPUT, d) == 0) ALooper_removeFd(NULL, fd);
      return ALOOPER_POLL_CALLBACK;
    }
    ident = g_lfds[i].ident;
    break;
  }
  mutexUnlock(&g_looper_lock);
  return ident;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  int ident;
  uint64_t deadline;

  looper_once();
  if (!sj_n_polls)
    printf("sj: first ALooper_pollOnce (timeout=%d) -- the glue loop is running\n",
           timeoutMillis);
  sj_n_polls++;
  sj_mark("ALooper_pollOnce");
  if (outFd)     *outFd = -1;
  if (outEvents) *outEvents = 0;
  if (outData)   *outData = NULL;

  deadline = armTicksToNs(armGetSystemTick()) +
             (timeoutMillis > 0 ? (uint64_t)timeoutMillis * 1000000ULL : 0);

  for (;;) {
    mutexLock(&g_looper_lock);
    if (g_wake_flag) { g_wake_flag = 0; mutexUnlock(&g_looper_lock); return ALOOPER_POLL_WAKE; }
    mutexUnlock(&g_looper_lock);

    /* Input is not backed by an fd -- sj_input.c holds its own ring -- so it
     * is checked directly rather than through poll_scan.
     *
     * outData MUST be set. android_native_app_glue does
     *     while (ALooper_pollAll(0, NULL, &events, (void**)&source) >= 0)
     *         if (source != NULL) source->process(app, source);
     * so reporting input with a NULL source means the event is never drained,
     * sj_input_pending() stays true, this keeps returning a non-negative
     * ident, and the inner while NEVER EXITS -- appUpdate is never reached and
     * the game freezes on the frame it last drew, spinning at millions of
     * polls per second. One screen touch was enough to trigger it. */
    if (sj_input_pending()) {
      void *src = sj_input_looper_data();

      /* Safety net independent of the above: if the consumer is not actually
       * finishing events, reporting them again cannot help. Drain them here
       * rather than spin. Losing input is bad; wedging the game is worse. */
      if (!src || sj_input_spinning()) {
        static int warned;
        AInputEvent *e;
        if (!warned) {
          warned = 1;
          printf("sj: WARNING input is not being drained (source=%p); "
                 "discarding to keep the glue loop alive\n", src);
        }
        while (AInputQueue_getEvent(NULL, &e) >= 0)
          AInputQueue_finishEvent(NULL, e, 0);
        continue;
      }

      if (outFd)     *outFd     = -1;
      if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
      if (outData)   *outData   = src;
      sj_input_note_dispatch();
      return sj_input_looper_ident();
    }

    /* Sensor samples. The game registered its queue with an ident and only
     * drains it when pollOnce hands that ident back; without this the tilt
     * queue is never read and steering does nothing at all. */
    if (sj_sensor_pending()) {
      if (outFd)     *outFd     = -1;
      if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
      if (outData)   *outData   = sj_sensor_looper_data();
      return sj_sensor_looper_ident();
    }

    ident = poll_scan(outFd, outEvents, outData);
    if (ident) return ident;

    /* Do not report "nothing to do" before the window exists: the glue would
     * fall straight through to appUpdate, which dereferences a ScreenManager
     * the game has not built yet. Capped so a real delivery failure is a slow
     * start with a warning rather than a hang. */
    if (!g_window_delivered) {
      uint64_t now = armTicksToNs(armGetSystemTick());
      if (!g_first_poll_ns) g_first_poll_ns = now;
      if (now - g_first_poll_ns < 5000000000ULL) { svcSleepThread(1000000ULL); continue; }
      printf("sj: WARNING no window after 5s; letting the game proceed\n");
      g_window_delivered = 1;
    }

    if (timeoutMillis == 0) return ALOOPER_POLL_TIMEOUT;
    if (timeoutMillis > 0 && armTicksToNs(armGetSystemTick()) >= deadline)
      return ALOOPER_POLL_TIMEOUT;

    /* Nothing ready. Sleep briefly rather than spinning: a blocking pollOnce
     * here is the glue thread idling between frames, and we must not burn a
     * core doing it. 1 ms keeps command latency well under a frame. */
    svcSleepThread(1000000ULL);
  }
}

/* Sonic Jump Fever's android_main polls with ALooper_pollAll, not pollOnce:
 *
 *     while ((ident = ALooper_pollAll(animating ? 0 : -1, NULL, &events,
 *                                     (void **)&source)) >= 0) {
 *         if (source) source->process(app, source);
 *         if (ident == 3) { ...ASensorEventQueue_getEvents... }
 *         if (app->destroyRequested) { ...; return; }
 *     }
 *
 * The NDK defines pollAll as pollOnce repeated for as long as it only ran
 * callbacks, returning the first real ident, a wake, a timeout or an error. */
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  for (;;) {
    int r = ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
    if (r != ALOOPER_POLL_CALLBACK) return r;
  }
}

/* --- configuration ---------------------------------------------------------
 * Only language and country are read. AConfiguration_fromAssetManager needs a
 * non-NULL manager to exist but never dereferences ours.
 * ------------------------------------------------------------------------ */

struct AConfigurationImpl { char lang[4]; char country[4]; };

AConfiguration *AConfiguration_new(void)
{
    struct AConfigurationImpl *c = calloc(1, sizeof(*c));
    if (c) { strcpy(c->lang, "en"); strcpy(c->country, "US"); }
    return (AConfiguration *)c;
}

void AConfiguration_delete(AConfiguration *c) { free(c); }

void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *am)
{
    struct AConfigurationImpl *cfg = (struct AConfigurationImpl *)c;
    u64 code = 0;
    (void)am;
    if (!cfg) return;

    /* setGetLanguageCode fails unless the set: service is open. Without this
     * it silently returns an error and every console reports English, which
     * looks like the game ignoring the system language rather than a bug
     * here. jni_fake.c does the same dance for its own locale lookup. */
    if (R_FAILED(setInitialize())) return;
    if (R_SUCCEEDED(setGetLanguageCode(&code))) {
        /* setGetLanguageCode returns a packed BCP-47 string like "en-GB". */
        const char *s = (const char *)&code;
        if (s[0] && s[1]) { cfg->lang[0] = s[0]; cfg->lang[1] = s[1]; cfg->lang[2] = 0; }
        if (s[2] == '-' && s[3]) { cfg->country[0] = s[3]; cfg->country[1] = s[4]; cfg->country[2] = 0; }
    }
    setExit();
}

void AConfiguration_getLanguage(AConfiguration *c, char *out)
{
    struct AConfigurationImpl *cfg = (struct AConfigurationImpl *)c;
    if (!out) return;
    out[0] = cfg ? cfg->lang[0] : 'e';
    out[1] = cfg ? cfg->lang[1] : 'n';
}

void AConfiguration_getCountry(AConfiguration *c, char *out)
{
    struct AConfigurationImpl *cfg = (struct AConfigurationImpl *)c;
    if (!out) return;
    out[0] = cfg ? cfg->country[0] : 'U';
    out[1] = cfg ? cfg->country[1] : 'S';
}

/* --- per-frame hook -------------------------------------------------------- */

/* Called once per frame from main(). The glue owns the render loop on its own
 * thread, so there is deliberately nothing to drive here -- we only yield so
 * the main thread does not starve it. Do not add rendering to this function. */
void android_native_frame(void)
{
    svcSleepThread(2000000ULL);   /* ~2 ms */
}

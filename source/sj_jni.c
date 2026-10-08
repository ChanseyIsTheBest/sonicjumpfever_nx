/* sj_jni.c -- the Java side of Sonic Jump Fever, answered natively.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * JNI_OnLoad resolves and caches these classes (confirmed by disassembly of
 * libsonicjumpfever.so 1.6.1):
 *
 *   com/sega/sonicjumpfever/Loader                      the activity
 *   com/sega/sonicjumpfever/musicplayer                 MediaPlayer wrapper
 *   com/sega/sonicjumpfever/BillingServiceInterface     IAP (AppStoreBridge)
 *   com/sega/sonicjumpfever/AndroidAnalytics            analytics
 *   com/sega/sonicjumpfever/Softlight                   URL fetcher
 *   com/sega/sonicjumpfever/GooglePlayServices          achievements
 *   com/sega/sonicjumpfever/AndroidAds                  MoPub (instance)
 *   com/sega/sonicjumpfever/external/ChartboostHelper   offer wall
 *   com/sega/sonicjumpfever/network/HTTPManager         HTTP (instance)
 *   com/sega/sonicjumpfever/network/HTTPRequest
 *   com/sega/sonicjumpfever/network/FacebookManager
 *   com/sega/sonicjumpfever/network/ImageDownloader
 *   com/sega/sonicjumpfever/push/PushNotifications
 *   com/sega/sonicjumpfever/push/LocalNotificationScheduler
 *   com/sega/sonicjumpfever/playUtils/License (+ LicenseListener, CryptoLib)
 *
 * android/app/NativeActivity.getFilesDir() and java/io/File.getPath() are
 * answered generically by jni_fake.c.
 *
 * The rule throughout: every asynchronous request is answered, even if the
 * answer is "no". An unanswered request leaves the engine waiting on a pending
 * flag, and with Fever's online systems that means a screen that never
 * settles. The servers for this game are long gone, so the honest answer to
 * every network question is "offline", and the game is built to carry on
 * without them.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>

#include "sj_jni.h"
#include "sj_music.h"
#include "jni_fake.h"
#include "so_util.h"
#include "config.h"
#include "sj_paths.h"
#include "error.h"
#include "sj_trace.h"
#include <switch.h>

extern so_module main_mod;

#define PKG "com/sega/sonicjumpfever/"

/* Native callbacks we drive. Resolved once after the module is loaded. Every
 * signature here was read off the exported function's own argument registers:
 * C calls happily through a mistyped function pointer, and on aarch64 a float
 * where an int was expected lands in the wrong register file entirely. */
typedef void (*fn_v)(void *env, void *thiz);
typedef void (*fn_i)(void *env, void *thiz, int32_t a);          /* jint/jboolean */
typedef void (*fn_j)(void *env, void *thiz, int64_t a);          /* jlong */
typedef void (*fn_str)(void *env, void *thiz, void *jstr);
typedef void (*fn_str_i)(void *env, void *thiz, void *jstr, int32_t v);
typedef void (*fn_str_str)(void *env, void *thiz, void *a, void *b);

static fn_i       cb_billing_supported;   /* (jboolean)          */
static fn_str_str cb_product_info_ok;     /* (sku, price)        */
static fn_str     cb_product_info_fail;   /* (sku)               */
static fn_str_i   cb_payment_failed;      /* (sku, jint code)    */
static fn_v       cb_restore_failed;      /* ()                  */
static fn_i       cb_is_connected;        /* (jboolean)          */
static fn_v       cb_fetch_finished;      /* ()                  */
static fn_v       cb_trigger_back;        /* ()                  */
static fn_i       cb_fb_sdk_init;         /* (jboolean loggedIn) */
static fn_i       cb_fb_session_state;    /* (jint SessionState) */
static fn_i       cb_fb_friends_done;     /* (jboolean success)  */
static fn_j       cb_http_failed;         /* (jlong HttpRequest*) */
static fn_j       cb_http_release;        /* (jlong HttpRequest*) */

/* --- deferred callbacks ----------------------------------------------------
 * Every BillingServiceNativeCallbacks entry point takes the store mutex, and
 * the engine calls into us (requestProductInfo, requestPurchase, ...) while
 * already holding it. Replying from inside the JNI handler would deadlock the
 * game thread against itself. On Android the Java side returns first and the
 * reply arrives later from another thread; queueing here and firing from our
 * main loop reproduces that ordering. Facebook and network replies go through
 * the same queue for the same reason: on Android they arrive asynchronously,
 * and the engine is written for that. HTTP results are the exception -- see
 * http_deliver below.
 * ------------------------------------------------------------------------ */
enum { DEF_V, DEF_I, DEF_STR, DEF_STR_I, DEF_STR_STR };

static void http_deliver(int from_pump);

typedef struct {
    void    *fn;
    int      kind;
    int32_t  ival;
    int64_t  lval;
    uint64_t not_before_ns;     /* fire no earlier than this */
    char     str[96];
    char     str2[32];
} Deferred;

#define DEFERRED_MAX 64
static Deferred g_def[DEFERRED_MAX];
static int      g_def_n;
static Mutex    g_def_lock;
static int      g_def_ready;

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

static void def_push_full(void *fn, int kind, const char *s, const char *s2,
                          int32_t v, int64_t lv, uint64_t delay_ns)
{
    if (!fn) return;
    if (!g_def_ready) { mutexInit(&g_def_lock); g_def_ready = 1; }
    mutexLock(&g_def_lock);
    if (g_def_n < DEFERRED_MAX) {
        Deferred *d = &g_def[g_def_n++];
        d->fn = fn; d->kind = kind; d->ival = v; d->lval = lv;
        d->not_before_ns = delay_ns ? now_ns() + delay_ns : 0;
        if (s)  snprintf(d->str,  sizeof(d->str),  "%s", s);
        else    d->str[0]  = '\0';
        if (s2) snprintf(d->str2, sizeof(d->str2), "%s", s2);
        else    d->str2[0] = '\0';
    } else {
        static int warned;
        if (!warned) { warned = 1; printf("sj_jni: WARNING deferred queue full\n"); }
    }
    mutexUnlock(&g_def_lock);
}

static void def_push(void *fn, int kind, const char *s, int32_t v)
{
    def_push_full(fn, kind, s, NULL, v, 0, 0);
}

void sj_jni_pump_deferred(void)
{
    Deferred batch[DEFERRED_MAX];
    int n = 0, i, keep = 0;
    uint64_t now;

    http_deliver(0);            /* only anything the engine stopped pumping */
    if (!g_def_ready || !g_def_n) return;
    now = now_ns();

    /* Copy out what is due under the lock and fire it outside: the callbacks
     * re-enter the engine and can queue more work. */
    mutexLock(&g_def_lock);
    for (i = 0; i < g_def_n; i++) {
        if (g_def[i].not_before_ns && g_def[i].not_before_ns > now)
            g_def[keep++] = g_def[i];
        else
            batch[n++] = g_def[i];
    }
    g_def_n = keep;
    mutexUnlock(&g_def_lock);

    for (i = 0; i < n; i++) {
        Deferred *d = &batch[i];
        switch (d->kind) {
        case DEF_V:     ((fn_v)d->fn)(fake_env, NULL); break;
        case DEF_I:     ((fn_i)d->fn)(fake_env, NULL, d->ival); break;
        case DEF_STR:   ((fn_str)d->fn)(fake_env, NULL, jni_make_string(d->str)); break;
        case DEF_STR_I: ((fn_str_i)d->fn)(fake_env, NULL,
                            jni_make_string(d->str), d->ival); break;
        case DEF_STR_STR:
            ((fn_str_str)d->fn)(fake_env, NULL, jni_make_string(d->str),
                                jni_make_string(d->str2));
            break;
        default: break;
        }
    }
}

/* --- HTTP requests ---------------------------------------------------------
 * sl::HttpManagerAndroid hands each request to Java as queueRequest(J) after
 * taking a reference on it, and calls Java's callbackPump()V from the game
 * thread -- the shape of a design where the Java worker queues results and the
 * pump delivers them on the engine's own thread. So a request is failed (as a
 * dead server would fail it) and released inside callbackPump, at least 1 s
 * after it was queued so a caller that retries on failure cannot spin.
 *
 * Should the engine ever stop pumping, the main loop delivers anything 3 s
 * overdue rather than leaving it pending for ever. */
#define HTTP_MAX 32
static struct { int64_t req; uint64_t due_ns; } g_http[HTTP_MAX];
static int   g_http_n;
static Mutex g_http_lock;        /* zero-initialised == unlocked */

static void http_queue(int64_t req)
{
    mutexLock(&g_http_lock);
    if (g_http_n < HTTP_MAX) {
        g_http[g_http_n].req = req;
        g_http[g_http_n].due_ns = now_ns() + 1000000000ULL;
        g_http_n++;
    } else {
        static int warned;
        if (!warned) { warned = 1; printf("sj_jni: WARNING HTTP queue full\n"); }
    }
    mutexUnlock(&g_http_lock);
}

static void http_deliver(int from_pump)
{
    int64_t due[HTTP_MAX];
    int n = 0, keep = 0, i;
    const uint64_t now = now_ns();
    const uint64_t grace = from_pump ? 0 : 3000000000ULL;

    mutexLock(&g_http_lock);
    for (i = 0; i < g_http_n; i++) {
        if (g_http[i].due_ns + grace <= now) due[n++] = g_http[i].req;
        else g_http[keep++] = g_http[i];
    }
    g_http_n = keep;
    mutexUnlock(&g_http_lock);

    for (i = 0; i < n; i++) {
        static int logged;
        if (logged < 4) {
            printf("sj_jni: HTTP request %llx failed (offline)%s\n",
                   (unsigned long long)due[i], from_pump ? "" : " [late, main loop]");
            logged++;
        }
        cb_http_failed(fake_env, NULL, due[i]);
        if (cb_http_release) cb_http_release(fake_env, NULL, due[i]);
    }
}

#define RESOLVE(var, type, name) \
    var = (type)so_try_find_addr_rx(&main_mod, name); \
    if (!var) printf("sj_jni: optional callback %s not present\n", name)

#define JNAME(cls, m) "Java_com_sega_sonicjumpfever_" cls "_" m

void sj_jni_resolve_callbacks(void)
{
    RESOLVE(cb_billing_supported, fn_i,
            JNAME("BillingServiceNativeCallbacks", "checkBillingSupportCallback"));
    RESOLVE(cb_product_info_ok, fn_str_str,
            JNAME("BillingServiceNativeCallbacks", "ProductInfoSuccess"));
    RESOLVE(cb_product_info_fail, fn_str,
            JNAME("BillingServiceNativeCallbacks", "ProductInfoFail"));
    RESOLVE(cb_payment_failed, fn_str_i,
            JNAME("BillingServiceNativeCallbacks", "PaymentFailed"));
    RESOLVE(cb_restore_failed, fn_v,
            JNAME("BillingServiceNativeCallbacks", "RestorePurchaseFailed"));
    RESOLVE(cb_is_connected, fn_i, JNAME("checkNetwork", "isConnectedCallBack"));
    RESOLVE(cb_fetch_finished, fn_v,
            JNAME("DownloadFilesTask", "fetchFileTaskFinished"));
    RESOLVE(cb_trigger_back, fn_v, JNAME("Loader", "triggerBack"));
    RESOLVE(cb_fb_sdk_init, fn_i,
            JNAME("network_FacebookManager", "onSDKInitialisedCallback"));
    RESOLVE(cb_fb_session_state, fn_i,
            JNAME("network_FacebookManager", "onSessionStateCallback"));
    RESOLVE(cb_fb_friends_done, fn_i,
            JNAME("network_FacebookManager", "onGetFriendsCompleteCallback"));
    RESOLVE(cb_http_failed, fn_j,
            JNAME("network_HTTPRequest", "requestFailedNative"));
    RESOLVE(cb_http_release, fn_j,
            JNAME("network_HTTPRequest", "releaseRequestNative"));
}

/* Called from main.c once the lifecycle has been delivered. */
void sj_jni_push_initial_state(void)
{
    printf("sj_jni: pushing initial state (offline, store %s)\n",
           SJ_STORE_ENABLED ? "supported" : "unsupported");

    /* checkBillingSupportCallback gates sl::store as a whole ("store
     * enabled/disabled" in the log). It is answered with SJ_STORE_ENABLED,
     * which is 0: with the store on, launches ended in the orange secure-
     * monitor screen right after "store enabled" (see config.h). Deferred:
     * it takes the store mutex. */
    def_push((void *)cb_billing_supported, DEF_I, NULL, SJ_STORE_ENABLED);

    /* Offline is the honest answer, and it is what keeps the profile, ad and
     * leaderboard paths from sitting on requests to dead servers. */
    def_push((void *)cb_is_connected, DEF_I, NULL, 0);
}

/* --------------------------------------------------------------------------
 * Back button.
 *
 * The Java activity forwarded onBackPressed to Loader.triggerBack(), which
 * only sets a flag android_main polls before calling appSystemBackButtonPressed
 * on the game thread -- so it is safe to call from here.
 *
 * At the home screen HomeScreen::onBackButton calls slRequestShutdown() with
 * no confirmation, which on Switch looks exactly like a crash. So work out
 * which screen would actually handle Back before sending it, by walking the
 * stack the way ScreenManager::onBackButtonPressed does (disassembled):
 *
 *     if (overlay at +192 && overlay->onBackButton()) return;
 *     for (i = count(+184) - 1; i >= 0; i--)         // screens at +24
 *         if (screens[i]->onBackButton()) return;
 *
 * onBackButton is vtable slot 23 (+184). Calling it to find out would act on
 * it, so instead recognise the screens that never handle Back: their slot 23
 * is literally "mov w0, #0; ret" (UIScreenBase's default, and InGameScreen's
 * -- which is why Back during a level falls through to whatever is beneath
 * it). The first screen that is not one of those is the one that will act; if
 * that is the HomeScreen, decline. The HomeScreen vtable and g_screenManager
 * are both exported symbols, so nothing here is a hardcoded address.
 * ------------------------------------------------------------------------ */
#define BACK_VSLOT_OFF 184          /* onBackButton: vtable index 23 */

/* The screen stack belongs to the game thread, and we read it from ours: a
 * screen popped at the wrong moment would leave us holding a stale pointer.
 * Check every pointer before following it -- the object must be in mapped,
 * readable memory, and its vtable and code must lie inside the game module --
 * so a badly timed press is skipped rather than faulting. */
static int readable(const void *p, size_t len)
{
    MemoryInfo mi;
    u32 pi;
    uintptr_t a = (uintptr_t)p;
    if (!p) return 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
    return a + len <= mi.addr + mi.size;
}

static int in_module(uintptr_t a)
{
    uintptr_t base = (uintptr_t)main_mod.load_virtbase;
    return a >= base && a < base + main_mod.load_size;
}

/* Does this screen's onBackButton always return false? -1 if the screen does
 * not look like a valid object (the caller then sends nothing). */
static int never_handles_back(const void *screen)
{
    const uintptr_t *vptr;
    const uint32_t *code;
    if (!readable(screen, 8)) return -1;
    vptr = *(const uintptr_t * const *)screen;
    if (!in_module((uintptr_t)vptr) || !readable(vptr, BACK_VSLOT_OFF + 8)) return -1;
    code = (const uint32_t *)vptr[BACK_VSLOT_OFF / 8];
    if (!in_module((uintptr_t)code) || !readable(code, 8)) return -1;
    return code[0] == 0x52800000u &&     /* mov w0, #0 */
           code[1] == 0xd65f03c0u;       /* ret        */
}

int sj_jni_send_back(void)
{
    static uintptr_t home_vptr, sm_slot;
    static int resolved;

    if (!cb_trigger_back) return 0;

    if (!resolved) {
        uintptr_t vt = so_try_find_addr_rx(&main_mod, "_ZTV10HomeScreen");
        resolved = 1;
        sm_slot = so_try_find_addr_rx(&main_mod, "g_screenManager");
        /* An Itanium vtable's address point is past the offset-to-top and
         * typeinfo words; that is what an object's vptr holds. */
        home_vptr = vt ? vt + 16 : 0;
        if (!home_vptr || !sm_slot)
            printf("sj: Back guard unavailable (HomeScreen vtable %p, "
                   "g_screenManager %p) -- B will be ignored\n",
                   (void *)vt, (void *)sm_slot);
    }
    if (!home_vptr || !sm_slot) return 0;

    {
        const uint8_t *sm = *(const uint8_t * const *)sm_slot;
        const void *chain[21];
        int n = 0, count, i;

        if (!sm) return 0;                     /* not built yet */
        if (!readable(sm, 200)) return 0;
        chain[n] = *(const void * const *)(sm + 192);
        if (chain[n]) n++;
        count = *(const int *)(sm + 184);
        if (count < 0 || count > 20) return 0; /* not what we expect: do nothing */
        for (i = count - 1; i >= 0; i--) {
            const void *s = *(const void * const *)(sm + 24 + (size_t)i * 8);
            if (s) chain[n++] = s;
        }

        for (i = 0; i < n; i++) {
            int skip = never_handles_back(chain[i]);  /* validates chain[i] */
            if (skip < 0) return 0;                   /* mid-change: try later */
            if (*(const uintptr_t *)chain[i] == home_vptr) {
                static int logged;
                if (logged < 3) {
                    printf("sj: B ignored -- it would reach the home screen, "
                           "where Back quits the game. Use HOME to leave.\n");
                    logged++;
                }
                return 0;
            }
            if (!skip) break;                         /* this one acts */
        }
    }
    cb_trigger_back(fake_env, NULL);
    return 1;
}

/* --------------------------------------------------------------------------
 * Method dispatch.
 *
 * jni_fake.c routes Call<Type>Method for our classes here. Match on method
 * name (and class where a name is shared); anything unrecognised is logged
 * once and gets a return-type-aware default.
 * ------------------------------------------------------------------------ */

static int streq(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static int is_cls(const char *cls, const char *tail)
{
    return cls && !strncmp(cls, PKG, sizeof(PKG) - 1) &&
           !strcmp(cls + sizeof(PKG) - 1, tail);
}

static void *str_ret(const char *s) { return jni_make_string(s ? s : ""); }

int sj_jni_owns_class(const char *cls)
{
    return cls && !strncmp(cls, PKG, sizeof(PKG) - 1);
}

/* The ISO-639 code Fever's strings::getSystemLanguage compares against. */
static const char *fever_lang(void)
{
    return jni_game_lang_code();
}

static int sj_jni_call_inner(const char *cls, const char *method,
                             const char *sig, const SjArgs *args, int64_t *out);

/* The first call of each method is logged on the way IN as well as out: a
 * call the console dies inside still leaves its line on the card. */
static void jni_log_enter(const char *cls, const char *m, const char *sig)
{
    static char  seen[160][160];
    static int   n;
    static Mutex lock;
    char key[160];
    int i;

    snprintf(key, sizeof(key), "%s::%s%s", cls ? cls : "?", m ? m : "?",
             sig ? sig : "");
    mutexLock(&lock);
    for (i = 0; i < n; i++)
        if (!strcmp(seen[i], key)) { mutexUnlock(&lock); return; }
    if (n < 160) snprintf(seen[n++], sizeof(seen[0]), "%s", key);
    mutexUnlock(&lock);
    printf("sj_jni: enter %s\n", key);
}

int sj_jni_call(const char *cls, const char *method, const char *sig,
                const SjArgs *args, int64_t *out)
{
    int64_t r = 0;
    int handled;
    sj_mark("JNI call");
    jni_log_enter(cls, method, sig);
    handled = sj_jni_call_inner(cls, method, sig, args, &r);
    if (out) *out = r;
    if (handled) sj_jni_log_call(cls, method, sig, (long long)r);
    return handled;
}

#define RET(v) do { if (out) *out = (int64_t)(v); return 1; } while (0)
#define RET_STR(s) do { if (out) *out = (int64_t)(uintptr_t)str_ret(s); return 1; } while (0)

static int sj_jni_call_inner(const char *cls, const char *method,
                             const char *sig, const SjArgs *args, int64_t *out)
{
    /* ---- isConnected()V, on whichever class asks -------------------------
     * Asynchronous: the answer arrives through checkNetwork_isConnectedCallBack.
     * The first hardware log showed the game calling it on Softlight, not
     * Loader -- netThreadFunc's performCheckConnectedToNetwork, every 100 ms --
     * so a Loader-only handler left it unanswered. Answer offline, always. */
    if (streq(method, "isConnected") && sig && !strcmp(sig, "()V")) {
        /* At most one answer per half second: it is asked ten times a second
         * and the answer never changes. */
        static uint64_t last;
        const uint64_t now = now_ns();
        if (!last || now - last > 500000000ULL) {
            last = now;
            def_push((void *)cb_is_connected, DEF_I, NULL, 0);
        }
        RET(0);
    }

    /* ---- musicplayer: the .m4a soundtrack --------------------------------
     * slPlayMusic -> PlayAudioFile("frontend.m4a", loop, restart).
     * slStopMusic -> StopAudio(), slSetPause -> setPause(Z),
     * slSetMusicVolume -> setVolume(F). */
    if (is_cls(cls, "musicplayer")) {
        if (streq(method, "PlayAudioFile")) {
            const char *name = sj_jni_arg_string(args, 0);
            int loop = args->count > 1 ? sj_jni_arg_int(args, 1) : 1;
            printf("sj_music: PlayAudioFile(\"%s\", loop=%d)\n", name, loop);
            sj_music_play(name, loop);
            RET(0);
        }
        if (streq(method, "StopAudio")) { sj_music_stop(); RET(0); }
        if (streq(method, "setPause")) {
            int p = args->count > 0 ? sj_jni_arg_int(args, 0) : 0;
            if (p) sj_music_pause(); else sj_music_resume();
            RET(0);
        }
        if (streq(method, "setVolume")) {
            float v = args->count > 0 ? sj_jni_arg_float(args, 0) : 1.0f;
            printf("sj_music: setVolume(%.3f)\n", v);
            sj_music_set_volume(v);
            RET(0);
        }
        if (streq(method, "isPlaying")) RET(sj_music_is_playing());
    }

    /* ---- Loader (the activity) ------------------------------------------- */
    if (is_cls(cls, "Loader")) {
        /* android_main caches getRotation()I before anything else and uses it
         * to remap the accelerometer. The panel never rotates. */
        if (streq(method, "getRotation")) RET(0);          /* ROTATION_0 */
        if (streq(method, "getApkFileName")) {
            /* NOT optional. initGame does
             *     ZipFile::openArchive(getApkFileName())
             *       -> appPreInitialise -> createAudio -> createNetwork
             *       -> slInitialise -> appInitialise
             * with every step gated on the last, and every resource path is
             * "apk:assets/...". */
            const char *apk = sj_apk();
            if (!apk || !*apk)
                fatal_error("No Sonic Jump Fever APK found in\n%s\n\nThe "
                            "engine reads its assets straight out of the APK, "
                            "so your own copy must sit next to the .nro.",
                            sj_home());
            printf("sj: getApkFileName -> %s\n", apk);
            RET_STR(apk);
        }
        if (streq(method, "getLanguage")) {
            const char *code = fever_lang();
            printf("sj: reporting language '%s'\n", code);
            RET_STR(code);
        }
        if (streq(method, "getLocaleISO3CountryCode")) RET_STR(jni_locale_iso3_country());
        /* (isConnected is answered above, for every class.) */
        /* No browser, no store page, no gallery on Switch. */
        if (streq(method, "openURL") || streq(method, "tryOpenURL") ||
            streq(method, "insertImage") || streq(method, "rateThisApp"))
            RET(0);
    }

    /* ---- BillingServiceInterface (sl::AppStoreBridge) ---------------------
     * Request/callback pairs: the Z return only means "request accepted"; the
     * real answer comes back through BillingServiceNativeCallbacks. */
    if (is_cls(cls, "BillingServiceInterface")) {
        if (streq(method, "checkBillingSupported")) {
            /* Fixed off (SJ_STORE_ENABLED, config.h). The handlers below
             * stay so that a store switched back on is still answered. */
            def_push((void *)cb_billing_supported, DEF_I, NULL, SJ_STORE_ENABLED);
            RET(SJ_STORE_ENABLED);
        }
        if (streq(method, "requestProductInfo")) {
            /* Succeed, so the shop lists its items. The price string is
             * cosmetic -- nothing can be bought here. */
            const char *sku = sj_jni_arg_string(args, 0);
            if (sku && *sku) {
                def_push_full((void *)cb_product_info_ok, DEF_STR_STR, sku, "--",
                              0, 0, 0);
            } else {
                def_push((void *)cb_product_info_fail, DEF_STR, sku, 0);
            }
            RET(1);
        }
        if (streq(method, "requestPurchase")) {
            const char *sku = sj_jni_arg_string(args, 0);
            printf("sj_iap: purchase of '%s' declined (no store on Switch)\n", sku);
            def_push((void *)cb_payment_failed, DEF_STR_I, sku, 0);
            RET(1);
        }
        if (streq(method, "restorePurchases")) {
            /* The in-game "Restore Purchases": there is nothing to restore
             * from. Owned upgrades (double rings, energy refill reducer) are
             * set through save_edit.txt instead. */
            def_push((void *)cb_restore_failed, DEF_V, NULL, 0);
            RET(1);
        }
        if (streq(method, "update")) RET(0);               /* polled per frame */
        RET(0);
    }

    /* ---- Softlight: fetchURLTo(url, path) ---------------------------------
     * getData(url, dest) downloads a file and the Java side then signals
     * DownloadFilesTask.fetchFileTaskFinished, which marks the engine's
     * current FetchFileThreadTask done. Nothing can be fetched, but the task
     * must still finish or its network thread waits on it. */
    if (is_cls(cls, "Softlight")) {
        if (streq(method, "getData"))
            def_push_full((void *)cb_fetch_finished, DEF_V, NULL, NULL, 0, 0,
                          100000000ULL);   /* 100 ms: after the task is set */
        RET(0);
    }

    /* ---- network/HTTPManager (instance) -----------------------------------
     * sl::HttpManagerAndroid creates one with <init>()V and hands every
     * request over as queueRequest(J), after taking a reference on it. The
     * Java side answered from its worker thread with requestFailedNative /
     * requestCompletedNative and then releaseRequestNative. Fail each request
     * after a short delay, as a dead server would, so a caller that retries
     * on failure cannot spin. */
    if (is_cls(cls, "network/HTTPManager")) {
        if (streq(method, "queueRequest")) {
            int64_t req = sj_jni_arg_long(args, 0);
            if (req && cb_http_failed) http_queue(req);
            RET(0);
        }
        /* The engine pumps this from its own thread; that is where the Java
         * side delivered finished requests, so that is where we fail them. */
        if (streq(method, "callbackPump")) { http_deliver(1); RET(0); }
        RET(0);                 /* pause / resume / quit */
    }

    /* ---- network/FacebookManager ------------------------------------------
     * ConnectedProfileManager sets its Facebook state to 2 ("initialising")
     * and calls FacebookInitialiseSDK; it stays there until the SDK reports
     * back. onSDKInitialisedCallback(false) moves it to 1, "logged out" --
     * which is the truth. A login attempt is answered with session state 5
     * (CLOSED_LOGIN_FAILED), which takes it down the logging-out path. */
    if (is_cls(cls, "network/FacebookManager")) {
        if (streq(method, "FacebookInitialiseSDK")) {
            def_push((void *)cb_fb_sdk_init, DEF_I, NULL, 0);
            RET(0);
        }
        if (streq(method, "getSessionState")) RET(0);      /* CREATED: no token */
        if (streq(method, "login")) {
            def_push((void *)cb_fb_session_state, DEF_I, NULL, 5);
            RET(0);
        }
        if (streq(method, "getFriends")) {
            def_push((void *)cb_fb_friends_done, DEF_I, NULL, 0);
            RET(0);
        }
        RET(0);                 /* logout / fetchUserData / sendInvite */
    }

    /* ---- GooglePlayServices (sl::gamification) ----------------------------
     * Achievements are tracked by the game itself; the Play Games mirror of
     * them simply is not there. Not signed in, nothing to show. */
    if (is_cls(cls, "GooglePlayServices")) {
        if (streq(method, "isLoggedIn")) RET(0);
        if (streq(method, "getAchievementProgress")) RET(0);   /* 0.0f */
        RET(0);
    }

    /* ---- AndroidAds (MoPub, instance + static) ----------------------------
     * Never ready, so no video-reward button is offered. ShowAd sets the
     * engine's m_advertActive, which sl::slPluginAds::update clears by
     * itself after 1.5 s, so an interstitial request cannot hang anything. */
    if (is_cls(cls, "AndroidAds")) {
        if (streq(method, "IsVideoReady") || streq(method, "IsVideoReadyIncentivised"))
            RET(0);
        RET(0);
    }

    /* ---- fire-and-forget services ------------------------------------------ */
    if (is_cls(cls, "AndroidAnalytics") || is_cls(cls, "external/ChartboostHelper") ||
        is_cls(cls, "network/ImageDownloader") || is_cls(cls, "push/PushNotifications") ||
        is_cls(cls, "push/LocalNotificationScheduler")) {
        if (streq(method, "getAdvertisingId")) RET_STR("");
        RET(0);
    }

    /* ---- Play Licensing ----------------------------------------------------
     * Present in the binary (sgInitiateLicenseCheck -> License.bindService)
     * but never called by this build. Answer "not bound" if it ever is. */
    if (is_cls(cls, "playUtils/License") || is_cls(cls, "playUtils/LicenseListener") ||
        is_cls(cls, "playUtils/CryptoLib"))
        RET(0);

    /* getAdvertisingId is looked up on a class we did not attribute
     * statically; answer it wherever it lands. */
    if (streq(method, "getAdvertisingId")) RET_STR("");

    /* Unhandled but ours: log once per distinct method, then return a default
     * that is safe for the return type. Zero is right for a primitive, but a
     * NULL jstring is not -- the original Sonic Jump abandoned a whole screen
     * on one -- so String returns get an empty string instead. */
    sj_jni_log_unhandled(cls, method, sig);
    if (out) {
        const char *ret = sig ? strchr(sig, ')') : NULL;
        if (ret && !strcmp(ret + 1, "Ljava/lang/String;"))
            *out = (int64_t)(uintptr_t)jni_make_string("");
        else
            *out = 0;
    }
    return 1;
}

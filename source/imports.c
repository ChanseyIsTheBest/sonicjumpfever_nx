/* imports.c -- resolver table for libsonicjumpfever.so
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 * Shim entries derived from the sonicjump_nx and angrybirdsjourney_nx
 * wrappers, (C) 2021 Andy Nguyen, fgsfds.
 *
 * Generated from the real import table of libsonicjumpfever.so (Sonic Jump
 * Fever 1.6.1, arm64-v8a) and then hand-checked. Re-run
 *     make check SO=/path/to/libsonicjumpfever.so
 * after editing: it diffs this table against the undefined symbols of the
 * actual library and fails on any gap.
 *
 * The library is linked BIND_NOW, so every one of these must resolve before
 * the first call into the module -- there is no lazy PLT fallback to catch a
 * mistake at runtime. A missing entry is tainted by so_resolve and reports its
 * own name if it is ever called.
 *
 * Coverage: 231 of 231 imports.
 *
 * What is different from the original Sonic Jump's table:
 *   - Fever imports __stack_chk_guard as DATA (an older NDK reads the canary
 *     from a global rather than TLS), clock_gettime with bionic clock ids,
 *     ALooper_pollAll instead of pollOnce, multitouch pointer queries, and
 *     dlopen/dlsym (GAbi++ looks up __android_log_print in liblog.so to
 *     print an abort message).
 *   - mmap is used once, by GAbi++, for a single anonymous page of exception
 *     globals -- see sj_mmap below.
 *   - It also needs libstdc++.so in DT_NEEDED but imports nothing from it: the
 *     C++ runtime (GAbi++, the unwinder, operator new) is linked statically.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
/* Deliberately NOT included: <syslog.h>, <semaphore.h>, <netdb.h>,
 * <arpa/inet.h>, <sys/socket.h>. devkitA64's newlib has no <syslog.h> at all,
 * and every symbol that would need the others resolves to a local shim or
 * into bsd_bridge.c, not to the system function. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"
#include "imports.h"
#include "android_native.h"
#include "sj_ndk.h"
#include "sj_input.h"
#include "sj_glue.h"
#include "sj_trace.h"
#include "nx_pointer.h"
#include "bsd_bridge.h"
#include "sj_sensor.h"
#include "sj_threads.h"

/* --- small local shims ---------------------------------------------------- */

/* exit() must neither kill the process (that skips the save-on-exit write) nor
 * return (it is declared noreturn, so there is nothing sane to return into).
 * sj_exit_park does the only correct third thing: flag the quit and park. */
static void sj_exit(int code) {
  (void)code;
  sj_exit_park();
}

static int sj_raise(int sig) {
  printf("sj: raise(%d) ignored\n", sig);
  return 0;
}

static unsigned sj_sleep(unsigned sec) {
  svcSleepThread((uint64_t)sec * 1000000000ULL);
  return 0;
}

/* bionic's assert(). The engine's own asserts are compiled in for EASTL and
 * tinyxml; the message is the single most useful line if one ever fires, so
 * put it in the log before aborting rather than dying silently. */
static void sj_assert2(const char *file, int line, const char *func,
                       const char *expr) {
  printf("sj: ASSERTION FAILED: %s\n    at %s:%d (%s)\n",
         expr ? expr : "?", file ? file : "?", line, func ? func : "?");
  fflush(stdout);
  abort();
}

/* --- paths ----------------------------------------------------------------
 * android_main builds the storage path as getFilesDir().getPath() + "/", and
 * getPath() already ends in '/' (see sj_paths.c for why), so the engine's save
 * paths come out with a double slash. fopen_fake collapses it; rename and
 * remove need the same treatment or a save written as "a//b.tmp" could not be
 * renamed over "a//b". */
static const char *norm_path(const char *in, char *buf, size_t cap) {
  size_t o = 0;
  if (!in || !strstr(in, "//")) return in;
  for (size_t i = 0; in[i] && o < cap - 1; i++) {
    if (in[i] == '/' && o > 0 && buf[o - 1] == '/') continue;
    buf[o++] = in[i];
  }
  buf[o] = '\0';
  return buf;
}

static int sj_rename(const char *from, const char *to) {
  char a[512], b[512];
  const char *na = norm_path(from, a, sizeof(a));
  const char *nb = norm_path(to, b, sizeof(b));
  /* POSIX rename replaces the destination; FAT on the SD card does not, and
   * the engine's save routine relies on the POSIX behaviour. */
  if (rename(na, nb) == 0) return 0;
  remove(nb);
  return rename(na, nb);
}

static int sj_remove(const char *path) {
  char a[512];
  return remove(norm_path(path, a, sizeof(a)));
}

/* --- mmap -----------------------------------------------------------------
 * The only caller is GAbi++'s __cxa_get_globals fallback, which asks for one
 * anonymous read/write page:
 *     mmap(NULL, 0x1000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
 * and gives it back with munmap at teardown. A page-aligned zeroed allocation
 * is exactly that. libc_shim's mmap_fake is deliberately not used: it reserves
 * a multi-hundred-megabyte arena on first use, sized for Unity. */
#define BIONIC_MAP_ANON 0x20
#define SJ_MAP_FAILED ((void *)-1)

static void *sj_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  void *p;
  (void)addr; (void)prot;
  if (!len) { errno = EINVAL; return SJ_MAP_FAILED; }
  len = (len + 0xfff) & ~(size_t)0xfff;
  p = memalign(0x1000, len);
  if (!p) { errno = ENOMEM; return SJ_MAP_FAILED; }
  memset(p, 0, len);
  if (!(flags & BIONIC_MAP_ANON) && fd >= 0) {
    /* Never seen from this library, but read the file in rather than hand
     * back zeroes if it ever happens: a private file mapping is a copy. */
    long cur = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, off, SEEK_SET) >= 0) {
      size_t got = 0;
      while (got < len) {
        long r = read(fd, (char *)p + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
    }
    if (cur >= 0) lseek(fd, cur, SEEK_SET);
  }
  return p;
}

static int sj_munmap(void *addr, size_t len) {
  (void)len;
  free(addr);
  return 0;
}

/* ===== bionic/libc shim bodies, from the sonicjump_nx / ABJ wrappers (MIT) ===== */
extern int *__errno(void);              // newlib

/* Android/bionic and devkitA64/newlib assign different numeric clock IDs for
 * the two clocks used most heavily by Unity:
 *
 *   Android CLOCK_REALTIME  = 0   newlib CLOCK_REALTIME  = 1
 *   Android CLOCK_MONOTONIC = 1   newlib CLOCK_MONOTONIC = 4
 *
 * CLOCK_BOOTTIME (7) is also translated for managed monotonic-time queries. */
static int clock_gettime_bionic(int android_id, struct timespec *tp) {
  if (!tp) {
    errno = EINVAL;
    return -1;
  }
  tp->tv_sec = 0;
  tp->tv_nsec = 0;

  clockid_t host_id;
  switch (android_id) {
    case 0:  /* CLOCK_REALTIME */
    case 5:  /* CLOCK_REALTIME_COARSE */
    case 8:  /* CLOCK_REALTIME_ALARM */
    case 11: /* CLOCK_TAI: realtime is the closest available clock */
      host_id = CLOCK_REALTIME;
      break;
    case 1: /* CLOCK_MONOTONIC */
    case 2: /* CLOCK_PROCESS_CPUTIME_ID: elapsed monotonic is sufficient here */
    case 3: /* CLOCK_THREAD_CPUTIME_ID */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
    case 7: /* CLOCK_BOOTTIME */
    case 9: /* CLOCK_BOOTTIME_ALARM */
      host_id = CLOCK_MONOTONIC;
      break;
    default:
      errno = EINVAL;
      return -1;
  }

  int rc = clock_gettime(host_id, tp);
  return rc;
}

// ---------------------------------------------------------------------------
// liblog
// ---------------------------------------------------------------------------

static int android_printf_discard(const char *fmt, ...) {
  if (!fmt) return 0;
  va_list ap;
  va_start(ap, fmt);
  const int result = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  return result;
}

/* The game's own logging.
 *
 * These formatted into NULL and threw the result away -- the engine has been
 * reporting its asset failures, script errors and state transitions all along
 * and we were deaf to every one of them. Anything the developers thought worth
 * printing is exactly what is wanted when a screen comes up empty.
 *
 * Routed to the log file with the engine's own priority and tag preserved, so
 * its messages are distinguishable from ours at a glance. */
static const char *log_prio_name(int prio) {
  switch (prio) {
    case 2: return "V"; case 3: return "D"; case 4: return "I";
    case 5: return "W"; case 6: return "E"; case 7: return "F";
    default: return "?";
  }
}

int __android_log_write(int prio, const char *tag, const char *text) {
  if (!text) return 0;
  printf("[%s/%s] %s%s", log_prio_name(prio), tag ? tag : "-", text,
         text[strlen(text) - 1] == '\n' ? "" : "\n");
  return 1;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  char line[1024];
  va_list ap;
  int n;
  if (!fmt) return 0;
  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return 0;
  __android_log_write(prio, tag, line);
  return n;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  char line[1024];
  va_list copy;
  int n;
  if (!fmt) return 0;
  va_copy(copy, va);
  n = vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);
  if (n < 0) return 0;
  __android_log_write(prio, tag, line);
  return n;
}
// ---------------------------------------------------------------------------
// stack protector / cxxabi
// ---------------------------------------------------------------------------

uint64_t __stack_chk_guard_fake = 0x0ull; /* match install_bionic_tls's zeroed tpidr+0x28 slot */
void __stack_chk_fail_fake(void) {  abort(); }

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

// ---------------------------------------------------------------------------
// pthread: bionic allocates the opaque types inline and zero-inits them, so we
// lazily back them with heap-allocated newlib objects stashed through the
// caller's pointer slot.
// ---------------------------------------------------------------------------

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (attr && *attr == 1); // bionic PTHREAD_MUTEX_RECURSIVE == 1
  int ret;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m; return 0;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) { pthread_mutex_destroy(*uid); free(*uid); *uid = NULL; }
  return 0;
}
static int ensure_mutex(pthread_mutex_t **uid) {
  // A real (already-initialized) handle is a heap pointer; anything small is a
  // bionic static initializer left in place: 0 = normal, 0x4000 = recursive,
  // 0x8000 = errorcheck, etc. Back ALL of those with a real newlib mutex.
  // (Was only handling 0 and 0x4000 -> a 0x8000 errorcheck static was treated
  // as a valid pointer and locked at address 0x8000: wild deref / hang.)
  if ((uintptr_t)*uid < 0x10000) {
    int recursive = ((uintptr_t)*uid == 0x4000);
    int a = 1;
    return pthread_mutex_init_fake(uid, recursive ? &a : NULL);
  }
  return 0;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  sj_mark("pthread_mutex_lock");
  if (ensure_mutex(uid) < 0) return -1;
  if (pthread_mutex_trylock(*uid) == 0) return 0;
  return pthread_mutex_lock(*uid);
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_trylock(*uid); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) { if (ensure_mutex(uid) < 0) return -1; return pthread_mutex_unlock(*uid); }
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  (void)abs;
  if (ensure_mutex(uid) < 0) return -1;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(*uid) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c; return 0;
}
static int ensure_cond(pthread_cond_t **cnd) {
  // like ensure_mutex: any small value is a bionic static cond initializer that
  // needs a real newlib object (was only treating 0 as needing init, so a
  // non-zero sentinel was waited/signalled on as a garbage pointer).
  if ((uintptr_t)*cnd < 0x10000) return pthread_cond_init_fake(cnd, NULL);
  return 0;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_broadcast(*cnd); }
int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_signal(*cnd); }
int pthread_cond_destroy_fake(pthread_cond_t **cnd) { if (cnd && (uintptr_t)*cnd >= 0x10000) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; } return 0; }
#define COND_WAIT_CAP_MS 16
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  /* Blocking wait. If the game parks here waiting for a worker that never
   * signals, the log just stops -- so note the first few and let the
   * heartbeat show whether the count keeps climbing. */
  sj_n_cond_waits++;
  sj_mark("pthread_cond_wait");
  if (sj_n_cond_waits <= 8)
    printf("sj: pthread_cond_wait #%u (blocking)\n", sj_n_cond_waits);
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  // Cap the UNTIMED wait too (not just the timed one below). Unity's engine
  // (job system, GfxDevice sync, PreloadManager) blocks the main thread in a
  // plain pthread_cond_wait; a raced/lost pthread_cond_signal -- or a
  // bionic-static-cond object mismatch across signal/wait -- would then hang the
  // whole engine forever (every worker idle, UnityMain parked in
  // svcWaitProcessWideKeyAtomic == the observed frame-2 deadlock). Waking every
  // ~16ms and returning as a spurious wakeup lets the caller re-check its
  // predicate (POSIX-legal: correct waiters always loop on the predicate), which
  // breaks a lost-wakeup stall without changing correct behaviour.
  struct timespec cap;
  clock_gettime(CLOCK_MONOTONIC, &cap);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;
  int r = pthread_cond_timedwait(*cnd, *mtx, &cap);
  return (r == ETIMEDOUT) ? 0 : r;   // timeout -> report as spurious wakeup
}
// Bound every timed cond-wait to at most COND_WAIT_CAP_MS. The .so libs (libc++
// std::condition_variable, and Swappy's frame pacer) compute an ABSOLUTE deadline
// against CLOCK_MONOTONIC, but newlib/libnx's pthread_cond_timedwait may measure
// "now" against a different clock -- a mismatch turns a ~16 ms vsync wait into an
// effectively infinite one (the Swappy hang: engine wedged in condvarWaitTimeout,
// frame counter frozen, black screen). Re-deriving the deadline as
// now(MONOTONIC)+min(requested, CAP) guarantees the wait can't exceed the cap
// regardless of which clock newlib uses, so the pacer hits its timeout fallback
// and keeps pacing. Spurious/early wakeups are POSIX-legal (every correct waiter
// re-checks its predicate), so this is safe in general.
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0) return -1;
  struct timespec now, cap;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  = now.tv_sec + (now.tv_nsec + add) / 1000000000L;
  cap.tv_nsec = (now.tv_nsec + add) % 1000000000L;
  // honor the caller's deadline if it's sooner than our cap; else clamp to cap
  const struct timespec *use = &cap;
  if (t && (t->tv_sec < cap.tv_sec ||
            (t->tv_sec == cap.tv_sec && t->tv_nsec <= cap.tv_nsec)))
    use = t;
  int r = pthread_cond_timedwait(*cnd, *mtx, use);
  return r;
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  if (__sync_lock_test_and_set(once, 1) == 0) (*init)();
  return 0;
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct {
  void *(*entry)(void *); void *arg; char name[48];
  uint8_t tls[BIONIC_TLS_SIZE];
} ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;   /* leaked on purpose: tpidr points into ts->tls */
  install_bionic_tls(ts->tls);
  /* Spread before any game code runs (see sj_threads.h): libnx starts every
   * pthread with its ideal core on the process default core. */
  sj_threads_spread_self(ts->name);
  return ts->entry(ts->arg);
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  extern so_module main_mod;
  const uintptr_t off = (uintptr_t)entry - (uintptr_t)main_mod.load_virtbase;
  sj_n_threads++;
  /* The offset names the function: 0x4feed8 is android_app_entry (the game
   * and its rendering), 0x282a5c netThreadFunc, in Fever 1.6.1. */
  printf("sj: pthread_create #%u entry=%p (game+0x%lx)\n", sj_n_threads, entry,
         (unsigned long)off);
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  snprintf(ts->name, sizeof(ts->name), "engine thread #%u (game+0x%lx)",
           sj_n_threads, (unsigned long)off);
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  if (stack < (2u << 20)) stack = 2u << 20; // 2 MB floor for the heavy engine threads
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  const int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);
  if (r != 0) { free(ts); return r; }
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  return pthread_join(thread, retval);
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
int pthread_kill_fake(pthread_t t, int sig) { (void)t; (void)sig; return 0; }

// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key.
// devkitA64 backs pthread keys with a tiny pool (~16 libnx TLS slots), but
// Unity's runtime creates dozens during init (46 call sites). The ~17th
// pthread_key_create returns EAGAIN, and libunity treats that as fatal
// (asserts the key was created, else BRK). bionic allows 128 keys; emulate
// that: one real key holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return EINVAL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}

/* NULL-safe string ops. Unity's SystemInfo device-detection feeds the device
 * model/manufacturer (NULL in our fake JNI) straight into compare/search libc
 * calls; raw newlib then deref-NULL-crashes (this is what killed strcasecmp).
 * Treat NULL as an empty string: comparisons are unequal, searches find nothing,
 * length is zero -- so the device checks all fall through harmlessly. */
static int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
static int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
static char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
static char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
static size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }

/* NDK entry points are declared in sj_ndk.h with exact signatures. */

static char *shader_fixups(const char *src) {
  const char *find = "monoCol = vec3";
  char *pos = strstr(src, find);
  if (!pos) return NULL;
  const char *repl = "vec3 monoCol = vec3";
  const size_t pre = (size_t)(pos - src), flen = strlen(find), rlen = strlen(repl);
  char *out = malloc(strlen(src) + (rlen - flen) + 1);
  if (!out) return NULL;
  memcpy(out, src, pre);
  memcpy(out + pre, repl, rlen);
  strcpy(out + pre + rlen, pos + flen);
  return out;
}
static void gl_ShaderSource_host(GLuint sh, GLsizei count, const GLchar *const *strs, const GLint *lens) {
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
  char *buf = malloc(total + 1);
  if (!buf) { glShaderSource(sh, count, strs, lens); return; }
  size_t off = 0;
  for (GLsizei i = 0; i < count; i++) {
    size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
    memcpy(buf + off, strs[i], l); off += l;
  }
  buf[off] = 0;
  char *fixed = shader_fixups(buf);
  const GLchar *final_src = fixed ? fixed : buf;
  glShaderSource(sh, 1, &final_src, NULL);
  free(buf);
  free(fixed);
}

// ---------------------------------------------------------------------------
// EGL surface-size fixup
// ---------------------------------------------------------------------------

// switch-mesa returns 0 for EGL_WIDTH/EGL_HEIGHT on the window surface even
// though it renders full-screen; the engine trusts that and sets a 0x0 viewport
// (nothing draws). Hand back the real panel size instead.
static EGLBoolean egl_QuerySurface_host(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (val) {
    if (attr == 0x3057 /*EGL_WIDTH*/  && *val <= 0) { *val = screen_width;  r = EGL_TRUE; }
    if (attr == 0x3056 /*EGL_HEIGHT*/ && *val <= 0) { *val = screen_height; r = EGL_TRUE; }
  }
  return r;
}

extern void android_native_draw_cursor(void);   /* docked cursor overlay (android_native_unity.c) */
/* --- clip-state tracing -----------------------------------------------------
 * LevelSelect builds its act buttons inside a UIScrollFrame and calls
 * enableVerticalScissoring(), so the buttons are drawn through a glScissor
 * rectangle. A scissor rect that lands off-screen or collapses to zero height
 * clips exactly the frame's contents while leaving the background visible --
 * which is the reported symptom, and it produces no error anywhere because
 * from GL's point of view nothing is wrong.
 *
 * These log the first few of each and then only report rectangles that would
 * clip everything away, so the cost is nil once things are working.
 * ------------------------------------------------------------------------ */
static GLint g_vp[4];

static void gl_Viewport_host(GLint x, GLint y, GLsizei w, GLsizei h) {
  static int n;
  g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h;
  if (n < 4) { printf("sj: glViewport(%d,%d,%d,%d)\n", x, y, w, h); n++; }
  glViewport(x, y, w, h);
}

static void gl_Scissor_host(GLint x, GLint y, GLsizei w, GLsizei h) {
  static int n, degenerate;
  if (n < 8) {
    printf("sj: glScissor(%d,%d,%d,%d)  [viewport %d,%d,%d,%d]\n",
           x, y, w, h, g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    n++;
  }
  /* Everything clipped: nothing drawn through this rect can ever appear. */
  if ((w <= 0 || h <= 0 ||
       x >= g_vp[0] + g_vp[2] || y >= g_vp[1] + g_vp[3] ||
       x + w <= g_vp[0] || y + h <= g_vp[1]) && degenerate < 6) {
    degenerate++;
    printf("sj: WARNING glScissor(%d,%d,%d,%d) clips everything away "
           "(viewport %d,%d,%d,%d) -- content drawn through it is invisible\n",
           x, y, w, h, g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
  }
  glScissor(x, y, w, h);
}

/* Set the window's size and rotation immediately before the surface is made.
 *
 * Doing it at startup did not work -- nwindowSetDimensions returned an error
 * and the log said only "could not rotate the window". The poinpy port shows
 * the right moment: it calls nwindowSetDimensions directly before
 * eglCreateWindowSurface, because that is when the window is ready to accept
 * a geometry and before anything has bound buffers to it.
 *
 * Here the game creates its own surface from inside the .so, so the hook has
 * to be this wrapper. The ANativeWindow the game passes IS the NWindow --
 * android_native_window() hands out nwindowGetDefault() -- so it can be used
 * directly.
 *
 * Transform values are Android's HAL constants, which libnx's nwindow layer
 * forwards: ROT_90 = 4, ROT_180 = 3, ROT_270 = 7. */
static EGLSurface egl_CreateWindowSurface_host(EGLDisplay dpy, EGLConfig cfg,
                                               EGLNativeWindowType win,
                                               const EGLint *attr) {
  NWindow *nw = (NWindow *)win;

  if (nw) {
    if (config.rotation) {
      const u32 rot = (config.rotation == 1) ? 4u : 7u;
      Result rt = nwindowSetTransform(nw, rot);
      Result rd = nwindowSetDimensions(nw, (u32)screen_width, (u32)screen_height);

      if (R_FAILED(rt))
        printf("sj: nwindowSetTransform(%u) failed rc=0x%08x\n", rot, (unsigned)rt);
      if (R_FAILED(rd))
        printf("sj: nwindowSetDimensions(%d,%d) failed rc=0x%08x\n",
               screen_width, screen_height, (unsigned)rd);
      if (R_SUCCEEDED(rt) && R_SUCCEEDED(rd))
        printf("sj: portrait surface %dx%d, transform %u\n",
               screen_width, screen_height, rot);
    } else {
      nwindowSetDimensions(nw, (u32)screen_width, (u32)screen_height);
    }
  }
  return eglCreateWindowSurface(dpy, cfg, win, attr);
}

static EGLBoolean egl_SwapBuffers_host(EGLDisplay d, EGLSurface s) {
  nxp_draw();                            /* cursor on top of the finished frame */
  sj_n_swaps++;                          /* heartbeat: is the render loop alive? */
  sj_mark("eglSwapBuffers");
  return eglSwapBuffers(d, s);
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  /* --- Switch-specific overrides (11) --- */
  { "__assert2",     (uintptr_t)&sj_assert2 },
  { "clock",         (uintptr_t)&sj_clock },
  { "clock_gettime", (uintptr_t)&clock_gettime_bionic },
  { "exit",          (uintptr_t)&sj_exit },
  { "gettimeofday",  (uintptr_t)&sj_gettimeofday },
  { "mmap",          (uintptr_t)&sj_mmap },
  { "munmap",        (uintptr_t)&sj_munmap },
  { "raise",         (uintptr_t)&sj_raise },
  { "remove",        (uintptr_t)&sj_remove },
  { "rename",        (uintptr_t)&sj_rename },
  { "sleep",         (uintptr_t)&sj_sleep },

  /* --- NDK: input, looper, window, sensors, configuration (29) --- */
  { "AConfiguration_delete",            (uintptr_t)&AConfiguration_delete },
  { "AConfiguration_fromAssetManager",  (uintptr_t)&AConfiguration_fromAssetManager },
  { "AConfiguration_getCountry",        (uintptr_t)&AConfiguration_getCountry },
  { "AConfiguration_getLanguage",       (uintptr_t)&AConfiguration_getLanguage },
  { "AConfiguration_new",               (uintptr_t)&AConfiguration_new },
  { "AInputEvent_getType",              (uintptr_t)&AInputEvent_getType },
  { "AInputQueue_attachLooper",         (uintptr_t)&AInputQueue_attachLooper },
  { "AInputQueue_detachLooper",         (uintptr_t)&AInputQueue_detachLooper },
  { "AInputQueue_finishEvent",          (uintptr_t)&AInputQueue_finishEvent },
  { "AInputQueue_getEvent",             (uintptr_t)&AInputQueue_getEvent },
  { "AInputQueue_preDispatchEvent",     (uintptr_t)&AInputQueue_preDispatchEvent },
  { "ALooper_addFd",                    (uintptr_t)&ALooper_addFd },
  { "ALooper_pollAll",                  (uintptr_t)&ALooper_pollAll },
  { "ALooper_prepare",                  (uintptr_t)&ALooper_prepare },
  { "AMotionEvent_getAction",           (uintptr_t)&AMotionEvent_getAction },
  { "AMotionEvent_getPointerCount",     (uintptr_t)&AMotionEvent_getPointerCount },
  { "AMotionEvent_getPointerId",        (uintptr_t)&AMotionEvent_getPointerId },
  { "AMotionEvent_getX",                (uintptr_t)&AMotionEvent_getX },
  { "AMotionEvent_getY",                (uintptr_t)&AMotionEvent_getY },
  { "ANativeActivity_finish",           (uintptr_t)&ANativeActivity_finish },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
  { "ASensorEventQueue_disableSensor",  (uintptr_t)&ASensorEventQueue_disableSensor },
  { "ASensorEventQueue_enableSensor",   (uintptr_t)&ASensorEventQueue_enableSensor },
  { "ASensorEventQueue_getEvents",      (uintptr_t)&ASensorEventQueue_getEvents },
  { "ASensorEventQueue_setEventRate",   (uintptr_t)&ASensorEventQueue_setEventRate },
  { "ASensorManager_createEventQueue",  (uintptr_t)&ASensorManager_createEventQueue },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue },
  { "ASensorManager_getDefaultSensor",  (uintptr_t)&ASensorManager_getDefaultSensor },
  { "ASensorManager_getInstance",       (uintptr_t)&ASensorManager_getInstance },

  /* --- OpenSL ES (6) --- */
  { "SL_IID_BUFFERQUEUE",  (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ENGINE",       (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY",         (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
  { "SL_IID_VOLUME",       (uintptr_t)&SL_IID_VOLUME },
  { "slCreateEngine",      (uintptr_t)&slCreateEngine },

  /* --- EGL (12) --- */
  { "eglChooseConfig",        (uintptr_t)&eglChooseConfig },
  { "eglCreateContext",       (uintptr_t)&eglCreateContext },
  { "eglCreateWindowSurface", (uintptr_t)&egl_CreateWindowSurface_host },
  { "eglDestroyContext",      (uintptr_t)&eglDestroyContext },
  { "eglDestroySurface",      (uintptr_t)&eglDestroySurface },
  { "eglGetConfigAttrib",     (uintptr_t)&eglGetConfigAttrib },
  { "eglGetDisplay",          (uintptr_t)&eglGetDisplay },
  { "eglInitialize",          (uintptr_t)&eglInitialize },
  { "eglMakeCurrent",         (uintptr_t)&eglMakeCurrent },
  { "eglQuerySurface",        (uintptr_t)&egl_QuerySurface_host },
  { "eglSwapBuffers",         (uintptr_t)&egl_SwapBuffers_host },
  { "eglTerminate",           (uintptr_t)&eglTerminate },

  /* --- OpenGL ES 2.0 (62) --- */
  { "glActiveTexture",           (uintptr_t)&glActiveTexture },
  { "glAttachShader",            (uintptr_t)&glAttachShader },
  { "glBindAttribLocation",      (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer",              (uintptr_t)&glBindBuffer },
  { "glBindTexture",             (uintptr_t)&glBindTexture },
  { "glBlendEquation",           (uintptr_t)&glBlendEquation },
  { "glBlendFuncSeparate",       (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData",              (uintptr_t)&glBufferData },
  { "glBufferSubData",           (uintptr_t)&glBufferSubData },
  { "glClear",                   (uintptr_t)&glClear },
  { "glClearColor",              (uintptr_t)&glClearColor },
  { "glCompileShader",           (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D",    (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram",           (uintptr_t)&glCreateProgram },
  { "glCreateShader",            (uintptr_t)&glCreateShader },
  { "glDeleteBuffers",           (uintptr_t)&glDeleteBuffers },
  { "glDeleteProgram",           (uintptr_t)&glDeleteProgram },
  { "glDeleteShader",            (uintptr_t)&glDeleteShader },
  { "glDeleteTextures",          (uintptr_t)&glDeleteTextures },
  { "glDepthMask",               (uintptr_t)&glDepthMask },
  { "glDetachShader",            (uintptr_t)&glDetachShader },
  { "glDisable",                 (uintptr_t)&glDisable },
  { "glDrawArrays",              (uintptr_t)&glDrawArrays },
  { "glDrawElements",            (uintptr_t)&glDrawElements },
  { "glEnable",                  (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glGenBuffers",              (uintptr_t)&glGenBuffers },
  { "glGenTextures",             (uintptr_t)&glGenTextures },
  { "glGetAttribLocation",       (uintptr_t)&glGetAttribLocation },
  { "glGetProgramInfoLog",       (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv",            (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog",        (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv",             (uintptr_t)&glGetShaderiv },
  { "glGetString",               (uintptr_t)&glGetString },
  { "glGetUniformLocation",      (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram",             (uintptr_t)&glLinkProgram },
  { "glScissor",                 (uintptr_t)&gl_Scissor_host },
  { "glShaderSource",            (uintptr_t)&gl_ShaderSource_host },
  { "glTexImage2D",              (uintptr_t)&glTexImage2D },
  { "glTexParameterf",           (uintptr_t)&glTexParameterf },
  { "glUniform1f",               (uintptr_t)&glUniform1f },
  { "glUniform1fv",              (uintptr_t)&glUniform1fv },
  { "glUniform1i",               (uintptr_t)&glUniform1i },
  { "glUniform1iv",              (uintptr_t)&glUniform1iv },
  { "glUniform2f",               (uintptr_t)&glUniform2f },
  { "glUniform2fv",              (uintptr_t)&glUniform2fv },
  { "glUniform2i",               (uintptr_t)&glUniform2i },
  { "glUniform2iv",              (uintptr_t)&glUniform2iv },
  { "glUniform3f",               (uintptr_t)&glUniform3f },
  { "glUniform3fv",              (uintptr_t)&glUniform3fv },
  { "glUniform3i",               (uintptr_t)&glUniform3i },
  { "glUniform3iv",              (uintptr_t)&glUniform3iv },
  { "glUniform4f",               (uintptr_t)&glUniform4f },
  { "glUniform4fv",              (uintptr_t)&glUniform4fv },
  { "glUniform4i",               (uintptr_t)&glUniform4i },
  { "glUniform4iv",              (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv",        (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv",        (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv",        (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram",              (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer",     (uintptr_t)&glVertexAttribPointer },
  { "glViewport",                (uintptr_t)&gl_Viewport_host },

  /* --- liblog / libdl / C++ runtime support (11) --- */
  { "__android_log_print",  (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "__cxa_atexit",         (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize",       (uintptr_t)&__cxa_finalize_fake },
  { "__errno",              (uintptr_t)&__errno },
  { "__stack_chk_fail",     (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard",    (uintptr_t)&__stack_chk_guard_fake },
  { "dl_iterate_phdr",      (uintptr_t)&so_dl_iterate_phdr },
  { "dlclose",              (uintptr_t)&dlclose_fake },
  { "dlopen",               (uintptr_t)&dlopen_fake },
  { "dlsym",                (uintptr_t)&dlsym_fake },

  /* --- pthread / semaphores (20) --- */
  { "pthread_attr_init",           (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_cond_broadcast",      (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy",        (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_init",           (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_wait",           (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_create",              (uintptr_t)&pthread_create_fake },
  { "pthread_getspecific",         (uintptr_t)&pthread_getspecific_fake },
  { "pthread_key_create",          (uintptr_t)&pthread_key_create_fake },
  { "pthread_key_delete",          (uintptr_t)&pthread_key_delete_fake },
  { "pthread_mutex_destroy",       (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init",          (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock",          (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock",        (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_once",                (uintptr_t)&pthread_once_fake },
  { "pthread_setspecific",         (uintptr_t)&pthread_setspecific_fake },
  { "sem_destroy",                 (uintptr_t)&sem_destroy_fake },
  { "sem_init",                    (uintptr_t)&sem_init_fake },
  { "sem_post",                    (uintptr_t)&sem_post_fake },
  { "sem_trywait",                 (uintptr_t)&sem_trywait_fake },

  /* --- stdio and file descriptors (26) --- */
  { "__sF",      (uintptr_t)&fake_sF },
  { "close",     (uintptr_t)&close_fake },
  { "fclose",    (uintptr_t)&fclose_fake },
  { "feof",      (uintptr_t)&feof_fake },
  { "ferror",    (uintptr_t)&ferror_fake },
  { "fflush",    (uintptr_t)&fflush_fake },
  { "fopen",     (uintptr_t)&fopen_fake },
  { "fprintf",   (uintptr_t)&fprintf_fake },
  { "fputc",     (uintptr_t)&fputc_fake },
  { "fputs",     (uintptr_t)&fputs_fake },
  { "fread",     (uintptr_t)&fread_fake },
  { "fseek",     (uintptr_t)&fseek_fake },
  { "ftell",     (uintptr_t)&ftell_fake },
  { "fwrite",    (uintptr_t)&fwrite_fake },
  { "lseek",     (uintptr_t)&z_lseek },
  { "open",      (uintptr_t)&open_fake },
  { "pipe",      (uintptr_t)&pipe_fake },
  { "printf",    (uintptr_t)&android_printf_discard },
  { "putchar",   (uintptr_t)&putchar },
  { "read",      (uintptr_t)&read_fake },
  { "snprintf",  (uintptr_t)&snprintf },
  { "sprintf",   (uintptr_t)&sprintf },
  { "sscanf",    (uintptr_t)&sscanf },
  { "vprintf",   (uintptr_t)&vprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "write",     (uintptr_t)&write_fake },

  /* --- sockets (6) --- */
  { "connect",       (uintptr_t)&connect_fake },
  { "gethostbyname", (uintptr_t)&nx_gethostbyname },
  { "inet_addr",     (uintptr_t)&nx_inet_addr },
  { "recv",          (uintptr_t)&recv_fake },
  { "send",          (uintptr_t)&send_fake },
  { "socket",        (uintptr_t)&socket_fake },

  /* --- memory (9) --- */
  { "free",     (uintptr_t)&free },
  { "malloc",   (uintptr_t)&malloc },
  { "memalign", (uintptr_t)&memalign },
  { "memchr",   (uintptr_t)&memchr },
  { "memcmp",   (uintptr_t)&memcmp },
  { "memcpy",   (uintptr_t)&memcpy },
  { "memmove",  (uintptr_t)&memmove },
  { "memset",   (uintptr_t)&memset },
  { "realloc",  (uintptr_t)&realloc },

  /* --- time (4) --- */
  { "gmtime",   (uintptr_t)&gmtime },
  { "strftime", (uintptr_t)&strftime },
  { "time",     (uintptr_t)&time },
  { "usleep",   (uintptr_t)&usleep },

  /* --- libc / libm (35) --- */
  { "abort",    (uintptr_t)&abort },
  { "acosf",    (uintptr_t)&acosf },
  { "atan2f",   (uintptr_t)&atan2f },
  { "atof",     (uintptr_t)&atof },
  { "atoi",     (uintptr_t)&atoi },
  { "cosf",     (uintptr_t)&cosf },
  { "expf",     (uintptr_t)&expf },
  { "fmodf",    (uintptr_t)&fmodf },
  { "frexp",    (uintptr_t)&frexp },
  { "isalnum",  (uintptr_t)&isalnum },
  { "isalpha",  (uintptr_t)&isalpha },
  { "isspace",  (uintptr_t)&isspace },
  { "log10f",   (uintptr_t)&log10f },
  { "longjmp",  (uintptr_t)&longjmp },
  { "modf",     (uintptr_t)&modf },
  { "pow",      (uintptr_t)&pow },
  { "powf",     (uintptr_t)&powf },
  { "rand",     (uintptr_t)&rand },
  { "setjmp",   (uintptr_t)&setjmp },
  { "sinf",     (uintptr_t)&sinf },
  { "sqrtf",    (uintptr_t)&sqrtf },
  { "srand",    (uintptr_t)&srand },
  { "stpcpy",   (uintptr_t)&stpcpy },
  { "strcat",   (uintptr_t)&strcat },
  { "strchr",   (uintptr_t)&z_strchr },
  { "strcmp",   (uintptr_t)&z_strcmp },
  { "strcpy",   (uintptr_t)&strcpy },
  { "strerror", (uintptr_t)&strerror },
  { "strlen",   (uintptr_t)&z_strlen },
  { "strncmp",  (uintptr_t)&z_strncmp },
  { "strncpy",  (uintptr_t)&strncpy },
  { "strstr",   (uintptr_t)&z_strstr },
  { "strtok",   (uintptr_t)&strtok },
  { "tolower",  (uintptr_t)&tolower },
  { "toupper",  (uintptr_t)&toupper },

};


size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

uintptr_t dynlib_find_export(const char *name) {
  DynLibFunction *f = so_find_import(dynlib_functions, (int)dynlib_numfunctions, name);
  return f ? f->func : 0;
}

void resolve_imports(so_module *mod) {
  /* taint_missing_imports = 1: an unresolved symbol becomes a trap rather than
   * a null call, so a gap reports its own name instead of crashing at 0x0. */
  so_resolve(mod, dynlib_functions, (int)dynlib_numfunctions, 1);
}

/* sj_jni.h -- Sonic Jump Fever's own Java classes. MIT licensed, see LICENSE. */
#ifndef SJ_JNI_H
#define SJ_JNI_H

#include <stdint.h>
#include <stdarg.h>

#define SJ_MAX_ARGS 8

typedef struct {
  char kind;            /* JNI signature letter; 'L' for any reference */
  union {
    int32_t  i;
    int64_t  j;
    float    f;
    double   d;
    void    *l;
  };
} SjValue;

typedef struct { int count; SjValue v[SJ_MAX_ARGS]; } SjArgs;

/* Walk `va` once, guided by `sig`, into `out`. Returns the argument count. */
int sj_jni_marshal(const char *sig, va_list va, SjArgs *out);

const char *sj_jni_arg_string(const SjArgs *a, int index);
int         sj_jni_arg_int(const SjArgs *a, int index);
int64_t     sj_jni_arg_long(const SjArgs *a, int index);
float       sj_jni_arg_float(const SjArgs *a, int index);
void        sj_jni_log_unhandled(const char *cls, const char *m, const char *sig);
/* Trace a handled call, once per distinct class::method. */
void        sj_jni_log_call(const char *cls, const char *m, const char *sig,
                            long long result);

/* True if `cls` is one of the com/sega/sonicjumpfever classes we implement.
 * jni_fake.c calls this to decide whether to route a call here. */
int sj_jni_owns_class(const char *cls);

/* Handle one call. Returns 1 if handled (result in *out), 0 to fall through
 * to jni_fake's generic behaviour. */
int sj_jni_call(const char *cls, const char *method, const char *sig,
                const SjArgs *args, int64_t *out);

void sj_jni_resolve_callbacks(void);
void sj_jni_push_initial_state(void);

/* Fire callbacks queued from inside JNI handlers. Call once per frame from the
 * main loop -- never from a JNI handler, which is the point. */
void sj_jni_pump_deferred(void);

/* B / Back. Delivered through the game's own Loader.triggerBack() export, the
 * way the Java activity forwarded onBackPressed. Returns 1 if it was sent, 0
 * if it was suppressed because Back would quit the game from the home screen. */
int sj_jni_send_back(void);

#endif

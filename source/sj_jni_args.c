/* sj_jni_args.c -- marshal JNI call arguments and log unhandled methods.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * WHY A MARSHALLER
 * ----------------
 * jni_fake.c dispatches with a va_list. A va_list can only be walked once and
 * only in order, so an indexed accessor over it is unsound: reading argument 1
 * before argument 0, or reading the same argument twice, is undefined
 * behaviour and on aarch64 silently returns the wrong register.
 *
 * So we walk the va_list exactly once, driven by the method signature, into a
 * small value array. After that, indexed access is safe and order-independent.
 * The contract is self-enforcing: the types come from the signature the engine
 * itself passed to GetMethodID, not from a convention we must remember to keep
 * in sync between two files.
 *
 * Default argument promotion matters here. Through an ellipsis, jboolean/jbyte/
 * jchar/jshort all arrive promoted to int, and jfloat arrives promoted to
 * double. Reading a float with va_arg(va, float) is the classic way to get
 * garbage out of exactly this kind of code.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "sj_jni.h"
#include "sj_trace.h"

const char *jni_string_utf(void *jstr);

/* Advance past one type token in a JNI signature. */
static const char *skip_type(const char *p)
{
    while (*p == '[') p++;               /* array dimensions */
    if (*p == 'L') {                     /* fully-qualified class name */
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
        return p;
    }
    if (*p) p++;                         /* primitive */
    return p;
}

int sj_jni_marshal(const char *sig, va_list va, SjArgs *out)
{
    const char *p = sig;

    out->count = 0;
    if (!sig) return 0;

    if (*p == '(') p++;
    while (*p && *p != ')' && out->count < SJ_MAX_ARGS) {
        SjValue *v = &out->v[out->count];
        const char *start = p;
        p = skip_type(p);
        v->kind = *start;

        switch (*start) {
        case 'Z': case 'B': case 'C': case 'S': case 'I':
            v->i = va_arg(va, int);              /* promoted to int */
            break;
        case 'J':
            v->j = va_arg(va, int64_t);
            break;
        case 'F':
            v->f = (float)va_arg(va, double);    /* promoted to double */
            break;
        case 'D':
            v->d = va_arg(va, double);
            break;
        default:                                 /* 'L' or '[': object ref */
            v->l = va_arg(va, void *);
            v->kind = 'L';
            break;
        }
        out->count++;
    }
    return out->count;
}

const char *sj_jni_arg_string(const SjArgs *a, int index)
{
    const char *s;
    if (!a || index < 0 || index >= a->count) return "";
    if (a->v[index].kind != 'L' || !a->v[index].l) return "";
    s = jni_string_utf(a->v[index].l);
    return s ? s : "";
}

int sj_jni_arg_int(const SjArgs *a, int index)
{
    if (!a || index < 0 || index >= a->count) return 0;
    switch (a->v[index].kind) {
    case 'J': return (int)a->v[index].j;
    case 'F': return (int)a->v[index].f;
    case 'D': return (int)a->v[index].d;
    case 'L': return a->v[index].l ? 1 : 0;
    default:  return a->v[index].i;
    }
}

/* Full 64 bits. HTTPManager.queueRequest(J) carries a native HttpRequest*
 * through a jlong, and truncating it through sj_jni_arg_int would hand the
 * engine back half a pointer. */
int64_t sj_jni_arg_long(const SjArgs *a, int index)
{
    if (!a || index < 0 || index >= a->count) return 0;
    switch (a->v[index].kind) {
    case 'J': return a->v[index].j;
    case 'F': return (int64_t)a->v[index].f;
    case 'D': return (int64_t)a->v[index].d;
    case 'L': return (int64_t)(intptr_t)a->v[index].l;
    default:  return a->v[index].i;
    }
}

float sj_jni_arg_float(const SjArgs *a, int index)
{
    if (!a || index < 0 || index >= a->count) return 0.0f;
    switch (a->v[index].kind) {
    case 'F': return a->v[index].f;
    case 'D': return (float)a->v[index].d;
    case 'J': return (float)a->v[index].j;
    case 'L': return 0.0f;
    default:  return (float)a->v[index].i;
    }
}

/* Log each distinct class::method once. During bring-up this list is the most
 * useful thing on the console: anything the game actually depends on shows up
 * here the first time it is called. */
#define SEEN_MAX 192
static char g_seen[SEEN_MAX][192];
static int  g_seen_n;

/* Trace every routed call, not just the unhandled ones.
 *
 * A stubbed method that returns the wrong value is indistinguishable from a
 * correct one in the log unless the call itself is recorded, and several of
 * these are gated on the module's g_javaSem -- a wrong answer there hangs the
 * game rather than crashing it, which is much harder to spot. Each distinct
 * class::method is logged once, so this stays quiet after the first frame. */
void sj_jni_log_call(const char *cls, const char *m, const char *sig,
                     long long result)
{
    char key[192];
    int i;

    snprintf(key, sizeof(key), "%s::%s%s", cls ? cls : "?", m ? m : "?",
             sig ? sig : "");
    for (i = 0; i < g_seen_n; i++)
        if (!strcmp(g_seen[i], key)) return;
    if (g_seen_n < SEEN_MAX) {
        snprintf(g_seen[g_seen_n], sizeof(g_seen[0]), "%s", key);
        g_seen_n++;
    }
    sj_trace_jni(key);
    printf("sj_jni: %s -> %lld\n", key, result);
}

void sj_jni_log_unhandled(const char *cls, const char *m, const char *sig)
{
    char key[192];
    int i;

    snprintf(key, sizeof(key), "%s::%s%s",
             cls ? cls : "?", m ? m : "?", sig ? sig : "");
    for (i = 0; i < g_seen_n; i++)
        if (!strcmp(g_seen[i], key)) return;
    if (g_seen_n < SEEN_MAX) {
        snprintf(g_seen[g_seen_n], sizeof(g_seen[0]), "%s", key);
        g_seen_n++;
    }
    printf("sj_jni: unhandled %s -> default 0\n", key);
}

/* sj_glue.h -- symbols the inherited shim layer expects. MIT, see LICENSE.
 *
 * These are declared here rather than left implicit so the compiler checks the
 * definitions in sj_glue.c against the externs in libc_shim.c / jni_fake.c.
 * A silent signature mismatch on aarch64 corrupts argument registers. */
#ifndef SJ_GLUE_H
#define SJ_GLUE_H

#include <stddef.h>

#include <time.h>

struct timeval;
clock_t sj_clock(void);
int   sj_gettimeofday(struct timeval *tv, void *tz);
int   sj_quit_via_exit(void);
void  sj_exit_park(void) __attribute__((noreturn));

void  android_get_orientation(float *x, float *y, float *z);
void  android_set_orientation(float x, float y, float z);
void  android_native_draw_cursor(void);
void *firebase_stub_lookup(const char *name);
void  journey_keyboard_set_result(const char *text, int canceled);

extern void  *fake_unityplayer_thiz;
extern void  *g_mmap_arena_base;
extern size_t g_mmap_arena_size;
extern size_t g_mmap_big_align;
extern int    g_overcommit;

#endif

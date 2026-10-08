/* unity_stubs.h -- inert replacements for the Unity/input hooks jni_fake.c
 * calls out to. MIT licensed, see LICENSE.
 *
 * jni_fake.c came from a Unity port and delegates unknown receivers to a
 * Unity object model and a MotionEvent pool. Sonic Jump has neither: it is a
 * NativeActivity title whose input arrives through AInputQueue (sj_input.c),
 * not through Java MotionEvent objects.
 *
 * Every hook here answers "not mine", so jni_fake's Unity branches are dead
 * and calls fall through to the Sonic Jump router (sj_jni.c) or to jni_fake's
 * own generic act_* handlers. Signatures match unity_jni.h / unity_input.h
 * exactly -- a mismatch would compile fine and then corrupt the argument
 * registers at the call site.
 */
#ifndef UNITY_STUBS_H
#define UNITY_STUBS_H

#include <stdarg.h>
#include <stdint.h>

int         unity_owns_class(const char *cls);
int         unity_owns_recv(void *recv);
const char *unity_recv_class(void *recv);
void       *unity_dispatch_object(void *recv, const void *id, va_list va);
uint64_t    unity_dispatch_int(void *recv, const void *id, va_list va);
float       unity_dispatch_float(void *recv, const void *id, va_list va);
void        unity_dispatch_void(void *recv, const void *id, va_list va);
int         unity_dispatch_object_a(void *recv, const void *id, const void *args, void **out);
int         unity_dispatch_int_a(void *recv, const void *id, const void *args, uint64_t *out);
int         unity_dispatch_float_a(void *recv, const void *id, const void *args, float *out);
int         unity_dispatch_void_a(void *recv, const void *id, const void *args);
int         unity_is_boxed(void *recv);
uint64_t    unity_boxed_int(void *recv);
float       unity_boxed_float(void *recv);
int         unity_isinstance(void *obj, const char *clazz);
void       *unity_motionevent_obtain(void *src);

int      input_owns_class(const char *cls);
int      input_owns_recv(const void *recv);
int      input_recv_is_motion(const void *recv);
int      input_instanceof_untyped(const void *recv);
uint64_t input_dispatch_int(void *recv, const void *id, va_list va);
float    input_dispatch_float(void *recv, const void *id, va_list va);

#endif

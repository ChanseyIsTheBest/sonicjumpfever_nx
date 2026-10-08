/* unity_stubs.c -- see unity_stubs.h. MIT licensed, see LICENSE. */

#include "unity_stubs.h"

/* --- Unity object model: we have none ------------------------------------ */
int         unity_owns_class(const char *cls)  { (void)cls;  return 0; }
int         unity_owns_recv(void *recv)        { (void)recv; return 0; }
const char *unity_recv_class(void *recv)       { (void)recv; return 0; }

void *unity_dispatch_object(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; return 0; }
uint64_t unity_dispatch_int(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; return 0; }
float unity_dispatch_float(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; return 0.0f; }
void unity_dispatch_void(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; }

/* The *_a variants take a jvalue array instead of a va_list and report
 * "handled" through the return value. 0 == not handled, leave *out alone. */
int unity_dispatch_object_a(void *recv, const void *id, const void *args, void **out)
{ (void)recv; (void)id; (void)args; (void)out; return 0; }
int unity_dispatch_int_a(void *recv, const void *id, const void *args, uint64_t *out)
{ (void)recv; (void)id; (void)args; (void)out; return 0; }
int unity_dispatch_float_a(void *recv, const void *id, const void *args, float *out)
{ (void)recv; (void)id; (void)args; (void)out; return 0; }
int unity_dispatch_void_a(void *recv, const void *id, const void *args)
{ (void)recv; (void)id; (void)args; return 0; }

int      unity_is_boxed(void *recv)   { (void)recv; return 0; }
uint64_t unity_boxed_int(void *recv)  { (void)recv; return 0; }
float    unity_boxed_float(void *recv){ (void)recv; return 0.0f; }

/* -1 means "not one of ours", which is what jni_fake expects for a pass. */
int unity_isinstance(void *obj, const char *clazz)
{ (void)obj; (void)clazz; return -1; }

/* Sonic Jump never constructs a Java MotionEvent -- its input comes through
 * AInputQueue. Returning the source keeps MotionEvent.obtain() harmless if a
 * support-library path ever reaches it. */
void *unity_motionevent_obtain(void *src) { return src; }

/* --- Java-side input events: likewise absent ----------------------------- */
int input_owns_class(const char *cls)        { (void)cls;  return 0; }
int input_owns_recv(const void *recv)        { (void)recv; return 0; }
int input_recv_is_motion(const void *recv)   { (void)recv; return 0; }
int input_instanceof_untyped(const void *recv){ (void)recv; return 0; }

uint64_t input_dispatch_int(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; return 0; }
float input_dispatch_float(void *recv, const void *id, va_list va)
{ (void)recv; (void)id; (void)va; return 0.0f; }

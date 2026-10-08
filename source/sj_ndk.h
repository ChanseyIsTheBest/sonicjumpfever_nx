/* sj_ndk.h -- prototypes for the Android NDK entry points implemented here.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * imports.c takes the address of each of these to build the resolver table, so
 * every one needs a visible declaration with the correct signature. Getting a
 * signature wrong here does not fail to compile -- it produces a table entry
 * that calls through with a mismatched ABI, which on aarch64 usually shows up
 * as garbage floats in AMotionEvent_getX. Keep these matched to the NDK.
 *
 * Implementations live in:
 *   sj_input.c        AInputQueue / AInputEvent / AMotionEvent / AKeyEvent
 *   sj_sensor.c       ASensorManager / ASensorEventQueue / ASensor
 *   android_native.c  ANativeWindow / ALooper / AConfiguration
 */

#ifndef SJ_NDK_H
#define SJ_NDK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "android_native.h"

/* --- input queue (sj_input.c) --------------------------------------------- */
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent);
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void    AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled);
void    AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                                 ALooper_callbackFunc callback, void *data);
void    AInputQueue_detachLooper(AInputQueue *queue);

int32_t AInputEvent_getType(const AInputEvent *event);
int32_t AInputEvent_getSource(const AInputEvent *event);
int32_t AInputEvent_getDeviceId(const AInputEvent *event);

int32_t AMotionEvent_getAction(const AInputEvent *event);
float   AMotionEvent_getX(const AInputEvent *event, size_t pointer_index);
float   AMotionEvent_getY(const AInputEvent *event, size_t pointer_index);
size_t  AMotionEvent_getPointerCount(const AInputEvent *event);
int32_t AMotionEvent_getPointerId(const AInputEvent *event, size_t index);
int64_t AMotionEvent_getEventTime(const AInputEvent *event);
int64_t AMotionEvent_getDownTime(const AInputEvent *event);
float   AMotionEvent_getPressure(const AInputEvent *event, size_t index);

int32_t AKeyEvent_getAction(const AInputEvent *event);
int32_t AKeyEvent_getKeyCode(const AInputEvent *event);
int32_t AKeyEvent_getFlags(const AInputEvent *event);
int32_t AKeyEvent_getMetaState(const AInputEvent *event);
int32_t AKeyEvent_getRepeatCount(const AInputEvent *event);

/* --- activity (sj_input.c) ------------------------------------------------- */
void ANativeActivity_finish(ANativeActivity *activity);

/* --- window (android_native.c) --------------------------------------------- */
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width,
                                         int32_t height, int32_t format);
int32_t ANativeWindow_getWidth(ANativeWindow *w);
int32_t ANativeWindow_getHeight(ANativeWindow *w);
int32_t ANativeWindow_getFormat(ANativeWindow *w);
void    ANativeWindow_acquire(ANativeWindow *w);
void    ANativeWindow_release(ANativeWindow *w);

/* --- looper (android_native.c) --------------------------------------------- */
ALooper *ALooper_prepare(int opts);
ALooper *ALooper_forThread(void);
void     ALooper_acquire(ALooper *l);
void     ALooper_release(ALooper *l);
void     ALooper_wake(ALooper *l);
int      ALooper_addFd(ALooper *l, int fd, int ident, int events,
                       ALooper_callbackFunc callback, void *data);
int      ALooper_removeFd(ALooper *l, int fd);
int      ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int      ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);

/* --- configuration (android_native.c) -------------------------------------- */
AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(AConfiguration *c);
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *am);
void AConfiguration_getLanguage(AConfiguration *c, char *out);
void AConfiguration_getCountry(AConfiguration *c, char *out);

/* --- sensors (sj_sensor.c) --------------------------------------------------
 * ASensorEvent is opaque to imports.c; only the addresses are taken. */
void   *ASensorManager_getInstance(void);
void   *ASensorManager_getInstanceForPackage(const char *pkg);
void   *ASensorManager_getDefaultSensor(void *manager, int type);
int     ASensorManager_getSensorList(void *manager, void ***list);
void   *ASensorManager_createEventQueue(void *manager, void *looper, int ident,
                                        void *callback, void *data);
int     ASensorManager_destroyEventQueue(void *manager, void *queue);
int     ASensorEventQueue_enableSensor(void *queue, const void *sensor);
int     ASensorEventQueue_disableSensor(void *queue, const void *sensor);
int     ASensorEventQueue_setEventRate(void *queue, const void *sensor, int32_t usec);
int     ASensorEventQueue_hasEvents(void *queue);
ssize_t ASensorEventQueue_getEvents(void *queue, void *events, size_t count);
const char *ASensor_getName(const void *s);
const char *ASensor_getVendor(const void *s);
int     ASensor_getType(const void *s);
float   ASensor_getResolution(const void *s);
int     ASensor_getMinDelay(const void *s);

#endif

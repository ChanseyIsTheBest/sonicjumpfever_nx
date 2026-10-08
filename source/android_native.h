/* Android NativeActivity compatibility types.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * These layouts match the Android NDK ABI consumed by the loaded libraries.
 */

#ifndef __ANDROID_NATIVE_H__
#define __ANDROID_NATIVE_H__

#include <stdint.h>
#include <stddef.h>

// --- NDK opaque types (we own the concrete instances) -----------------------
typedef struct ANativeWindow  ANativeWindow;   // == libnx NWindow* at runtime
typedef struct AInputQueue    AInputQueue;
typedef struct AInputEvent    AInputEvent;
typedef struct ALooper        ALooper;
typedef struct AConfiguration AConfiguration;
typedef struct AAssetManager  AAssetManager;
typedef struct ANativeActivity ANativeActivity;

typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

// --- ANativeActivityCallbacks (NDK android/native_activity.h layout) --------
typedef struct ANativeActivityCallbacks {
  void  (*onStart)(ANativeActivity *activity);
  void  (*onResume)(ANativeActivity *activity);
  void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
  void  (*onPause)(ANativeActivity *activity);
  void  (*onStop)(ANativeActivity *activity);
  void  (*onDestroy)(ANativeActivity *activity);
  void  (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
  void  (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
  void  (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
  void  (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
  void  (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
  void  (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
  void  (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
  void  (*onContentRectChanged)(ANativeActivity *activity, const void *rect);
  void  (*onConfigurationChanged)(ANativeActivity *activity);
  void  (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void       *vm;     // JavaVM*
  void       *env;    // JNIEnv*
  void       *clazz;  // the MyNativeActivity jobject
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t     sdkVersion;
  void       *instance;       // glue stashes its android_app* here
  AAssetManager *assetManager;
  const char *obbPath;
};

typedef void ANativeActivity_createFunc(ANativeActivity *activity, void *savedState, size_t savedStateSize);

// --- host control API (used by main.c) --------------------------------------

// initialise the fake-fd / looper layer. Call once before ANativeActivity_onCreate.
void android_native_init(void);

// The fake-fd layer backing the glue's command pipe is declared in fakefd.h,
// which is the single source of truth for it. It used to be duplicated here;
// two declarations of the same API drift.

// build the ANativeActivity the glue's onCreate consumes. Paths must outlive it.
ANativeActivity *android_make_activity(void *vm, void *env, void *clazz,
                                       AAssetManager *am,
                                       const char *internalPath,
                                       const char *externalPath,
                                       const char *obbPath);

// the single ANativeWindow handed to onNativeWindowCreated (wraps the default
// libnx NWindow). The engine passes it straight to eglCreateWindowSurface.
ANativeWindow *android_native_window(void);

// the AInputQueue handed to onInputQueueCreated; main.c injects HID into it.
AInputQueue *android_input_queue(void);

// --- input injection (called from the main/UI thread) -----------------------
// action: AMOTION_EVENT_ACTION_* ; coords are in window pixels.
void android_inject_motion(int32_t action, int pointer_count,
                           const int32_t *ids, const float *xs, const float *ys);
// action: AKEY_EVENT_ACTION_DOWN/UP ; keycode: AKEYCODE_*
void android_inject_key(int32_t action, int32_t keycode);

// --- sonicjumpfever_nx additions -------------------------------------------------
void android_native_update_mode(void);
uint32_t android_native_width(void);
uint32_t android_native_height(void);
// call once per frame from main(): yields to the engine's render thread
void android_native_frame(void);
// map a physical screen point into the engine's portrait space (pillarboxed)
void android_window_to_game(float *x, float *y);
// where the portrait image lands inside the landscape framebuffer
void android_game_viewport(int *x, int *y, int *w, int *h);
// set by ANativeActivity_finish (sj_input.c); polled by the main loop
int sj_finish_requested(void);
/* Call once onNativeWindowCreated has been delivered; until then the looper
 * refuses to report an empty queue (see ALooper_pollOnce). */
void android_native_window_delivered(void);

#endif

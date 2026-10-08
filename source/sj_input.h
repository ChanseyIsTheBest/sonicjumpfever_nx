/* sj_input.h -- NDK input queue emulation. MIT licensed, see LICENSE. */
#ifndef SJ_INPUT_H
#define SJ_INPUT_H
#include <stdint.h>
#include "android_native.h"

#define SJ_MOTION_DOWN   0
#define SJ_MOTION_UP     1
#define SJ_MOTION_MOVE   2
#define SJ_MOTION_CANCEL 3
#define SJ_KEY_DOWN      0
#define SJ_KEY_UP        1

void sj_input_init(void);
int  sj_input_pending(void);
/* The ident and poll-source the glue registered with AInputQueue_attachLooper;
 * ALooper_pollOnce must hand both back or the glue drops every input event. */
int   sj_input_looper_ident(void);
void *sj_input_looper_data(void);
/* Called by pollOnce each time it hands input to the consumer, and used to
 * detect a consumer that never drains (which otherwise wedges the glue). */
void sj_input_note_dispatch(void);
int  sj_input_spinning(void);
/* Heartbeat visibility: how many events the consumer has finished, and how
 * many reports have gone by without it finishing anything. */
unsigned sj_input_finishes(void);
unsigned sj_input_dispatch_stall(void);
/* Reads performed by the game's own handler -- proof it looked at the event,
 * not merely that the queue was drained. */
extern unsigned sj_n_getType, sj_n_getAction, sj_n_getXY;
/* Coordinates are in window pixels, already mapped to the game's surface. */
void sj_inject_touch(int32_t action, float x, float y);
void sj_inject_key(int32_t action, int32_t keycode);
/* Drop all queued/dispatched events (focus loss, so no touch survives suspend). */
void sj_input_reset(void);

/* Android keycode the game acts on: KEYCODE_BACK. Its handler only fires on
 * the UP action with FLAG_CANCELED clear, so inject a DOWN then an UP. */
#define SJ_KEYCODE_BACK 4
#endif

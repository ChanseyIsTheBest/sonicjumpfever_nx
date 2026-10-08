/* sj_input.c -- a real AInputQueue / AMotionEvent / AKeyEvent implementation.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * None of the reference wrappers implement the NDK input queue. Angry Birds
 * Journey stubs every one of these entry points to return "no events", because
 * Unity takes input through its own JNI path and never touches AInputQueue.
 *
 * Sonic Jump is a plain NativeActivity title: AInputQueue *is* its only input
 * path. So this is written from scratch.
 *
 * The engine's loop looks like:
 *
 *     ALooper_pollOnce(...)                  -> we say "input source ready"
 *     while (AInputQueue_getEvent(q,&e)>=0)  -> pop from our ring
 *         if (AInputQueue_preDispatchEvent(q,e)) continue;
 *         handled = engine_handle(e);
 *         AInputQueue_finishEvent(q,e,handled);
 *
 * so events must stay alive between getEvent() and finishEvent(). We hand out
 * pointers into a fixed slot pool and only recycle a slot on finishEvent().
 *
 * Sonic Jump Fever's handler is multitouch-aware (disassembled from the
 * onInputEvent callback android_main installs):
 *
 *     action = AMotionEvent_getAction(e);  index = (action >> 8) & 0xff;
 *     id     = AMotionEvent_getPointerId(e, index);
 *     DOWN / POINTER_DOWN        -> appTouchBegan(id, getX/Y(e, index))
 *     UP / POINTER_UP / CANCEL   -> appTouchEnded(id, ...)
 *     MOVE -> for i < getPointerCount(e): appTouchMoved(getPointerId(e,i), ...)
 *
 * with ids above 4 ignored, and every non-motion event declined. We deliver a
 * single pointer, id 0, index 0: main.c arbitrates the touchscreen, cursor and
 * jump buttons down to one owner, which is all the game needs (tap to jump,
 * tilt to steer) and keeps began/ended strictly paired. Key events are never
 * consumed by this game -- Back arrives through Loader.triggerBack instead,
 * see sj_jni_send_back.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>

#include "sj_input.h"
#include "android_native.h"
#include "sj_ndk.h"
#include "config.h"

/* --- NDK constants (android/input.h) -------------------------------------- */
#define AINPUT_EVENT_TYPE_KEY            1
#define AINPUT_EVENT_TYPE_MOTION         2

#define AMOTION_EVENT_ACTION_MASK        0xff
#define AMOTION_EVENT_ACTION_DOWN        0
#define AMOTION_EVENT_ACTION_UP          1
#define AMOTION_EVENT_ACTION_MOVE        2
#define AMOTION_EVENT_ACTION_CANCEL      3

#define AKEY_EVENT_ACTION_DOWN           0
#define AKEY_EVENT_ACTION_UP             1

#define AINPUT_SOURCE_TOUCHSCREEN    0x00001002
#define AINPUT_SOURCE_KEYBOARD       0x00000101

/* --- event pool ----------------------------------------------------------- */
/* 64 slots is far more than one frame can generate at 60 Hz with one finger,
 * but overflow must not corrupt state, so push_event() drops on full. */
#define EVENT_SLOTS 64

struct FakeInputEvent {
    int32_t  type;        /* AINPUT_EVENT_TYPE_*                    */
    int32_t  action;      /* read by AMotionEvent/AKeyEvent_getAction */
    int32_t  source;
    int32_t  keycode;
    int32_t  flags;
    int32_t  meta;
    int64_t  down_time;
    int64_t  event_time;
    float    x, y;
    int32_t  in_use;      /* 0 = free, 1 = queued, 2 = dispatched   */
};

static struct FakeInputEvent g_slots[EVENT_SLOTS];
static int32_t  g_queue[EVENT_SLOTS];
static int      g_head, g_tail, g_count;
static Mutex    g_lock;
static int      g_ready;

/* One singleton queue object; the engine only ever sees this pointer. */
static int      g_queue_token;

/* android_native_app_glue calls
 *     AInputQueue_attachLooper(q, looper, LOOPER_ID_INPUT, NULL,
 *                              &app->inputPollSource);
 * and its loop then does
 *     while (ALooper_pollAll(0, NULL, &events, (void**)&source) >= 0)
 *         if (source != NULL) source->process(app, source);
 *
 * so the ident AND the data pointer both have to come back out of pollOnce.
 * An earlier version discarded them and reported LOOPER_ID_INPUT with a NULL
 * source, which the glue silently skips -- events would pile up in the ring
 * and never be dispatched or finished, i.e. touch simply would not work. */
static int   g_attached_ident = 2;      /* LOOPER_ID_INPUT */
static void *g_attached_data;

int   sj_input_looper_ident(void) { return g_attached_ident; }
void *sj_input_looper_data(void)  { return g_attached_data; }

/* Spin detection.
 *
 * pollOnce reports input; the consumer is supposed to drain it with
 * getEvent/finishEvent. If it reports many times running without a single
 * finishEvent, the consumer is not draining and reporting again will never
 * help -- it just spins the glue loop and starves the frame. Counting
 * dispatches against finishes catches that regardless of the cause. */
static unsigned g_finishes;
static unsigned g_last_finishes;
static unsigned g_reports_since_progress;

/* Called each time pollOnce hands input to the consumer.
 *
 * The first version of this compared (reports - finishes) > 64 on unsigned
 * counters. That is wrong in the ordinary case, not a corner case: one poll
 * report drains a whole batch, so finishes routinely exceeds reports, the
 * subtraction wraps to ~4.29 billion, and the check fires immediately. The
 * "safety net" then discarded every touch -- and since SplashScreen::update
 * only advances on TouchMonitor::isTouchActive(), that alone kept the game on
 * the logo for ever.
 *
 * What actually indicates a stuck consumer is reports piling up with *no*
 * finishes in between, so measure exactly that and never subtract counters. */
void sj_input_note_dispatch(void)
{
    if (g_finishes != g_last_finishes) {
        g_last_finishes = g_finishes;      /* the consumer is draining */
        g_reports_since_progress = 0;
    } else {
        g_reports_since_progress++;
    }
}

int sj_input_spinning(void)
{
    /* 256 consecutive reports with not one event finished. At the 60 Hz the
     * glue polls at that is four seconds of a consumer doing nothing, which no
     * legitimate burst resembles. */
    return g_reports_since_progress > 256;
}

unsigned sj_input_finishes(void) { return g_finishes; }
unsigned sj_input_dispatch_stall(void) { return g_reports_since_progress; }

void sj_input_init(void)
{
    if (g_ready) return;
    mutexInit(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    g_head = g_tail = g_count = 0;
    g_ready = 1;
}

AInputQueue *android_input_queue(void)
{
    sj_input_init();
    return (AInputQueue *)&g_queue_token;
}

int sj_input_pending(void)
{
    int n;
    if (!g_ready) return 0;
    mutexLock(&g_lock);
    n = g_count;
    mutexUnlock(&g_lock);
    return n;
}

static int64_t now_ns(void)
{
    return (int64_t)armTicksToNs(armGetSystemTick());
}

/* Claim a free slot, fill it, and queue it. Caller holds no lock. */
static void push_event(const struct FakeInputEvent *src)
{
    int i, slot = -1;

    sj_input_init();
    mutexLock(&g_lock);

    for (i = 0; i < EVENT_SLOTS; i++) {
        if (!g_slots[i].in_use) { slot = i; break; }
    }
    /* Full. Dropping a MOVE is invisible; dropping an UP would strand the
     * engine in a permanent touch-down state, so evict the oldest queued MOVE
     * to make room rather than dropping the new event blindly. */
    if (slot < 0) {
        if (src->type == AINPUT_EVENT_TYPE_MOTION &&
            (src->action & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_MOVE) {
            mutexUnlock(&g_lock);
            return;
        }
        for (i = 0; i < g_count; i++) {
            int q = g_queue[(g_head + i) % EVENT_SLOTS];
            if (g_slots[q].type == AINPUT_EVENT_TYPE_MOTION &&
                (g_slots[q].action & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_MOVE) {
                slot = q;
                /* compact the ring around the evicted entry */
                for (; i < g_count - 1; i++)
                    g_queue[(g_head + i) % EVENT_SLOTS] =
                        g_queue[(g_head + i + 1) % EVENT_SLOTS];
                g_tail = (g_tail - 1 + EVENT_SLOTS) % EVENT_SLOTS;
                g_count--;
                break;
            }
        }
    }
    if (slot < 0) { mutexUnlock(&g_lock); return; }

    g_slots[slot] = *src;
    g_slots[slot].in_use = 1;
    g_queue[g_tail] = slot;
    g_tail = (g_tail + 1) % EVENT_SLOTS;
    g_count++;

    mutexUnlock(&g_lock);
}

/* --- injection API (called from the main thread each frame) ---------------- */

void sj_inject_touch(int32_t action, float x, float y)
{
    static int64_t s_down_time;
    struct FakeInputEvent e;

    memset(&e, 0, sizeof(e));
    e.type       = AINPUT_EVENT_TYPE_MOTION;
    e.action     = action;
    e.source     = AINPUT_SOURCE_TOUCHSCREEN;
    e.x          = x;
    e.y          = y;
    e.event_time = now_ns();
    if (action == AMOTION_EVENT_ACTION_DOWN) s_down_time = e.event_time;
    e.down_time  = s_down_time;
    push_event(&e);
}

void sj_inject_key(int32_t action, int32_t keycode)
{
    struct FakeInputEvent e;
    memset(&e, 0, sizeof(e));
    e.type       = AINPUT_EVENT_TYPE_KEY;
    e.action     = action;
    e.source     = AINPUT_SOURCE_KEYBOARD;
    e.keycode    = keycode;
    e.event_time = now_ns();
    e.down_time  = e.event_time;
    push_event(&e);
}

/* --- NDK entry points consumed by libsonicjumpgame.so --------------------- */

int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent)
{
    int slot;
    (void)queue;
    if (!g_ready) return -1;

    mutexLock(&g_lock);
    if (g_count == 0) { mutexUnlock(&g_lock); return -1; }
    slot  = g_queue[g_head];
    g_head = (g_head + 1) % EVENT_SLOTS;
    g_count--;
    g_slots[slot].in_use = 2;            /* dispatched: do not recycle yet */
    mutexUnlock(&g_lock);

    if (outEvent) *outEvent = (AInputEvent *)&g_slots[slot];
    return 0;
}

/* Android uses this to give the IME first refusal on key events. We have no
 * IME, so nothing is ever pre-dispatched. Returning non-zero here would make
 * the engine skip the event entirely. */
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event)
{
    (void)queue; (void)event;
    return 0;
}

void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled)
{
    struct FakeInputEvent *e = (struct FakeInputEvent *)event;
    (void)queue; (void)handled;
    if (!e) return;
    mutexLock(&g_lock);
    e->in_use = 0;                       /* slot returns to the pool */
    g_finishes++;                        /* proves the consumer is draining */
    mutexUnlock(&g_lock);
}

/* The glue attaches the queue to a looper so pollOnce can report it. Our
 * looper reports the input source unconditionally when events are pending
 * (see android_native.c), so there is nothing to wire up here. */
void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident,
                              ALooper_callbackFunc callback, void *data)
{
    (void)queue; (void)looper; (void)callback;
    if (ident > 0) g_attached_ident = ident;
    g_attached_data = data;
}

void AInputQueue_detachLooper(AInputQueue *queue)
{
    AInputEvent *e;
    (void)queue;
    /* The glue detaches before re-attaching on APP_CMD_INPUT_CHANGED, and on
     * pause it detaches without re-attaching. Keeping the old poll source
     * would let pollOnce hand events to a consumer that is no longer listening
     * -- they would never be finished, and the backlog would look like a stuck
     * consumer. Forget the source and drop anything queued: input received
     * while nothing is attached is meaningless anyway. */
    g_attached_data = NULL;
    while (AInputQueue_getEvent(NULL, &e) >= 0)
        AInputQueue_finishEvent(NULL, e, 0);
}

/* Drop every queued and dispatched event. Used when focus is lost, so a press
 * held at the moment of suspend cannot reappear as a stuck touch on resume. */
void sj_input_reset(void)
{
    int i;
    if (!g_ready) return;
    mutexLock(&g_lock);
    for (i = 0; i < EVENT_SLOTS; i++) g_slots[i].in_use = 0;
    g_head = g_tail = g_count = 0;
    mutexUnlock(&g_lock);
}

/* Proof-of-consumption counters.
 *
 * The glue finishing an event only proves it drained the queue -- if
 * app->onInputEvent is NULL it drains and discards without the game ever
 * looking. These count actual reads by the game's handler, which is the only
 * thing that distinguishes "delivered" from "delivered and consumed". */
unsigned sj_n_getType, sj_n_getAction, sj_n_getXY;

int32_t AInputEvent_getType(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    sj_n_getType++;
    return e ? e->type : 0;
}

int32_t AInputEvent_getSource(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->source : 0;
}

int32_t AInputEvent_getDeviceId(const AInputEvent *event)
{
    (void)event;
    return 0;
}

/* --- motion accessors ----------------------------------------------------- */

int32_t AMotionEvent_getAction(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    int32_t a = e ? e->action : 0;
    if (sj_n_getAction < 6)
        printf("sj: game read AMotionEvent_getAction -> %d  (0=DOWN 1=UP 2=MOVE)\n", a);
    sj_n_getAction++;
    return a;
}

float AMotionEvent_getX(const AInputEvent *event, size_t pointer_index)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    sj_n_getXY++;
    if (!e || pointer_index != 0) return 0.0f;
    return e->x;
}

float AMotionEvent_getY(const AInputEvent *event, size_t pointer_index)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    if (!e || pointer_index != 0) return 0.0f;
    return e->y;
}

/* Fever calls both. One pointer, always id 0 at index 0 -- see the header.
 * The action we inject never carries a pointer index in bits 8..15, so the
 * game's (action >> 8) & 0xff is always 0 and getX/getY(e, 0) apply. */
size_t AMotionEvent_getPointerCount(const AInputEvent *e)
{
    const struct FakeInputEvent *ev = (const struct FakeInputEvent *)e;
    return (ev && ev->type == AINPUT_EVENT_TYPE_MOTION) ? 1 : 0;
}
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i)
{
    (void)e; (void)i;
    return 0;
}
int64_t AMotionEvent_getEventTime(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->event_time : 0;
}
int64_t AMotionEvent_getDownTime(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->down_time : 0;
}
float AMotionEvent_getPressure(const AInputEvent *e, size_t i)
{
    const struct FakeInputEvent *ev = (const struct FakeInputEvent *)e;
    if (!ev || i != 0) return 0.0f;
    /* A touchscreen reports 1.0 while down. Some engines gate taps on this. */
    return (ev->action & AMOTION_EVENT_ACTION_MASK) == AMOTION_EVENT_ACTION_UP ? 0.0f : 1.0f;
}

/* --- key accessors -------------------------------------------------------- */

int32_t AKeyEvent_getAction(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->action : AKEY_EVENT_ACTION_DOWN;
}

int32_t AKeyEvent_getKeyCode(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->keycode : 0;
}

int32_t AKeyEvent_getFlags(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->flags : 0;
}

int32_t AKeyEvent_getMetaState(const AInputEvent *event)
{
    const struct FakeInputEvent *e = (const struct FakeInputEvent *)event;
    return e ? e->meta : 0;
}

int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { (void)e; return 0; }

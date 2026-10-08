/* sj_sensor.c -- ASensor emulation: synthesise an accelerometer for tilt steering.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * Sonic Jump steers by tilting the phone. The binary imports the whole
 * ASensorManager / ASensorEventQueue family, and the string table contains
 * "HintIcon_PhoneTilt". Angry Birds Journey stubs all of this to return
 * "no sensor" -- fine for Unity, fatal here: with no accelerometer the player
 * cannot steer at all.
 *
 * Three tilt sources, selectable in config.txt:
 *
 *   TILT_STICK   left stick -> synthetic gravity vector      (default)
 *   TILT_GYRO    real Joy-Con accelerometer via hidGetSixAxisSensorStates
 *   TILT_BOTH    whichever moved most recently
 *
 * TILT_STICK is the default on purpose. Real six-axis is faithful and feels
 * good in handheld, but is unusable docked with a Pro Controller resting flat
 * on a table -- there is no meaningful tilt to read.
 *
 * WHAT THE ENGINE EXPECTS
 * -----------------------
 * An Android accelerometer reports acceleration in m/s^2 including gravity, in
 * device coordinates. At rest, a phone held upright in portrait reads about
 * (0, +9.81, 0). Tilting left/right moves gravity into x. So for a tilt angle
 * theta the engine wants roughly:
 *
 *     x = -G * sin(theta)
 *     y =  G * cos(theta)
 *     z =  0
 *
 * Feeding raw stick deflection straight into x (a common shortcut) gives the
 * wrong magnitude and makes steering feel twitchy near centre and mushy at the
 * extremes, so we map through an actual angle.
 */

#include <math.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t: ASensorEventQueue_getEvents returns it */
#include <switch.h>

#include "sj_sensor.h"
#include "sj_ndk.h"
#include "sj_trace.h"
#include "config.h"
#include <stdio.h>

#define GRAVITY            9.80665f
#define ASENSOR_TYPE_ACCELEROMETER 1

/* Maximum simulated tilt at full stick deflection. 35 degrees is about as far
 * as a person actually tilts a phone while playing; going higher just makes
 * the usable stick range smaller. */
#define MAX_TILT_DEG       35.0f

/* ASensorEvent, NDK layout (android/sensor.h). The engine reads .acceleration
 * out of the union at offset 16, so the leading fields must be exact. */
typedef struct {
    int32_t version;        /* sizeof(ASensorEvent) */
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        float data[16];
        struct { float x, y, z; float _pad; } acceleration;
    } u;
    int32_t flags;
    int32_t reserved1[3];
} ASensorEvent;

static int   g_sensor_token;      /* the ASensor* we hand out   */
static int   g_manager_token;
static int   g_queue_token;
static int   g_enabled;
static float g_x, g_y = GRAVITY, g_z;
static int   g_have_event;
static int   g_mode = TILT_STICK;
static int   g_calibrated;
static float g_ref_x;

void sj_sensor_set_mode(int mode) { g_mode = mode; g_calibrated = 0; }
int  sj_sensor_get_mode(void)     { return g_mode; }

/* Convert a normalised axis value (-1..1) into a gravity vector. */
static void tilt_to_gravity(float tilt, float *out_x, float *out_y)
{
    float theta;
    if (config.tilt_invert) tilt = -tilt;
    if (tilt >  1.0f) tilt =  1.0f;
    if (tilt < -1.0f) tilt = -1.0f;
    theta  = tilt * (MAX_TILT_DEG * (float)M_PI / 180.0f);
    *out_x = -GRAVITY * sinf(theta);
    *out_y =  GRAVITY * cosf(theta);
}

/* Called once per frame from the main loop, before pumping the engine. */
void sj_sensor_update(void)
{
    float stick_tilt = 0.0f;
    int   got_stick  = 0;
    int   got_gyro   = 0;
    float gx = 0.0f, gy = GRAVITY, gz = 0.0f;

    if (!g_enabled) return;

    if (g_mode == TILT_STICK || g_mode == TILT_BOTH) {
        HidAnalogStickState st = padGetStickPos(sj_pad(), 0);
        /* JOYSTICK_MAX is 32767; deadzone keeps a resting stick from drifting. */
        float v = (float)st.x / 32767.0f;
        if (fabsf(v) > 0.12f) {
            /* rescale past the deadzone so the usable range stays full-width */
            float s = (fabsf(v) - 0.12f) / (1.0f - 0.12f);
            stick_tilt = (v < 0.0f) ? -s : s;
            /* Scale by the configured sensitivity, then clamp: past 100% the
             * stick reaches full lean before full deflection, which is the
             * point -- but it must not report more lean than a real phone
             * could produce. */
            stick_tilt *= (float)config.stick_sens / 100.0f;
            if (stick_tilt >  1.0f) stick_tilt =  1.0f;
            if (stick_tilt < -1.0f) stick_tilt = -1.0f;
            got_stick  = 1;
        } else {
            stick_tilt = 0.0f;
            got_stick  = 1;   /* centred stick is still a valid reading */
        }
    }

    if (g_mode == TILT_GYRO || g_mode == TILT_BOTH) {
        HidSixAxisSensorState six;
        memset(&six, 0, sizeof(six));
        if (sj_read_sixaxis(&six)) {
            /* The Joy-Con's six-axis reported as an Android accelerometer.
             *
             * libnx gives acceleration in g; Android wants m/s^2. The axes do
             * not line up either: a Joy-Con's +y runs along the rail towards
             * the shoulder buttons, which is the phone's -z, and +z points out
             * of the face, which is the phone's +y. So a controller tilted
             * left/right produces the same gravity vector a phone would. */
            {
                /* Rotate in the DEVICE'S SCREEN PLANE, before mapping to
                 * Android's axes.
                 *
                 * The console's x runs across the screen and y up it; z points
                 * out of the face and takes no part in a screen rotation. The
                 * previous version rotated gx against gy *after* the mapping,
                 * and gy is derived from acceleration.z -- so it swung the
                 * out-of-plane axis into the steering and tilt went wrong in a
                 * way that got worse the more the console was tilted.
                 *
                 * With the picture turned 90 CW the console is held 90 CCW to
                 * read it, so what is "right" on screen is the console's +y,
                 * and what is "up" on screen is its -x. */
                float ax = six.acceleration.x;
                float ay = six.acceleration.y;
                float az = six.acceleration.z;
                float rx = ax, ry = ay;

                if (config.rotation == 1) {        /* render rotated 90 CW  */
                    rx =  ay; ry = -ax;
                } else if (config.rotation == 2) { /* render rotated 90 CCW */
                    rx = -ay; ry =  ax;
                }

                gx = -rx * GRAVITY;   /* across the screen -- this steers */
                gy =  az * GRAVITY;   /* out of the face, rotation-invariant */
                gz =  ry * GRAVITY;   /* up the screen */
            }

            /* Calibrate to however the controller is being held.
             *
             * A phone tilt game assumes a neutral pose of "flat, facing you".
             * Nobody holds a Joy-Con like that -- held naturally it sits at
             * twenty or thirty degrees, which as a raw gravity vector is a
             * large constant steering bias: Sonic drifts hard to one side and
             * the controls feel broken rather than merely offset.
             *
             * So the first sample after the sensor is enabled becomes the
             * neutral reference and tilt is measured from there. */
            if (!g_calibrated) {
                g_ref_x = gx;
                g_calibrated = 1;
                printf("sj: tilt calibrated; holding this pose is now neutral "
                       "(ref x = %.2f m/s^2)\n", g_ref_x);
            }
            gx -= g_ref_x;

            if (config.tilt_invert) gx = -gx;
            got_gyro = 1;
        }
    }

    if (g_mode == TILT_GYRO && got_gyro) {
        g_x = gx; g_y = gy; g_z = gz;
    } else if (g_mode == TILT_BOTH && got_gyro && fabsf(stick_tilt) < 0.05f) {
        /* stick at rest: defer to the physical controller */
        g_x = gx; g_y = gy; g_z = gz;
    } else if (got_stick) {
        tilt_to_gravity(stick_tilt, &g_x, &g_y);
        g_z = 0.0f;
    }

    g_have_event = 1;
}

/* Drop the neutral reference so the next sample re-establishes it. */
void sj_sensor_recalibrate(void) { g_calibrated = 0; }

void sj_sensor_get(float *x, float *y, float *z)
{
    if (x) *x = g_x;
    if (y) *y = g_y;
    if (z) *z = g_z;
}

/* --- NDK entry points ------------------------------------------------------ */

void *ASensorManager_getInstance(void) { return &g_manager_token; }

void *ASensorManager_getInstanceForPackage(const char *pkg)
{
    (void)pkg;
    return &g_manager_token;
}

/* Report exactly one sensor: the accelerometer. Returning NULL here (as the
 * ABJ stub does) makes the game conclude the device has no tilt input. */
void *ASensorManager_getDefaultSensor(void *manager, int type)
{
    (void)manager;
    if (type == ASENSOR_TYPE_ACCELEROMETER) return &g_sensor_token;
    return NULL;
}

int ASensorManager_getSensorList(void *manager, void ***list)
{
    static void *sensors[1];
    (void)manager;
    sensors[0] = &g_sensor_token;
    if (list) *list = sensors;
    return 1;
}

/* The ident the game registered its sensor queue with, and the poll source it
 * passed (NULL here -- it drains the queue itself rather than via a callback).
 *
 * This has to come back out of ALooper_pollOnce. android_main's loop is
 *     while ((ident = ALooper_pollOnce(...)) >= 0) {
 *         if (source) source->process(app, source);
 *         if (ident == LOOPER_ID_USER) { ...ASensorEventQueue_getEvents... }
 *     }
 * so the sensor is only ever read when pollOnce reports that exact ident.
 * Discarding it meant the queue was never drained and NO tilt input reached
 * the game -- neither the synthesised stick vector nor the real six-axis. */
static int   g_queue_ident = 3;      /* LOOPER_ID_USER, the usual value */
static void *g_queue_data;

int   sj_sensor_looper_ident(void) { return g_queue_ident; }
void *sj_sensor_looper_data(void)  { return g_queue_data; }

static unsigned g_reports_since_drain;

int sj_sensor_pending(void)
{
    if (!g_enabled || !g_have_event) return 0;

    /* Same wedge risk as the input queue: android_main's inner loop runs
     * while pollOnce returns >= 0, so reporting a sample the game never
     * collects would spin it forever and the game would freeze mid-frame. If
     * the ident is wrong, or the game stops draining for any other reason,
     * drop the sample rather than lock up. */
    if (++g_reports_since_drain > 64) {
        static int warned;
        if (!warned) {
            warned = 1;
            printf("sj: WARNING sensor samples reported %u times with no "
                   "getEvents; dropping them to keep the loop alive (is the "
                   "queue ident right?)\n", g_reports_since_drain);
        }
        g_have_event = 0;
        g_reports_since_drain = 0;
        return 0;
    }
    return 1;
}

void *ASensorManager_createEventQueue(void *manager, void *looper, int ident,
                                      void *callback, void *data)
{
    (void)manager; (void)looper; (void)callback;
    if (ident > 0) g_queue_ident = ident;
    g_queue_data = data;
    printf("sj: sensor queue registered with looper ident %d\n", g_queue_ident);
    return &g_queue_token;
}

int ASensorManager_destroyEventQueue(void *manager, void *queue)
{
    (void)manager; (void)queue;
    g_enabled = 0;
    return 0;
}

int ASensorEventQueue_enableSensor(void *queue, const void *sensor)
{
    (void)queue; (void)sensor;
    g_enabled = 1;
    g_calibrated = 0;          /* re-reference on resume */
    printf("sj: accelerometer enabled (tilt_mode=%d; 0=stick 1=gyro 2=both)\n",
           g_mode);
    return 0;                 /* 0 == success; ABJ returned -1 (failure) */
}

int ASensorEventQueue_disableSensor(void *queue, const void *sensor)
{
    (void)queue; (void)sensor;
    g_enabled = 0;
    return 0;
}

int ASensorEventQueue_setEventRate(void *queue, const void *sensor, int32_t usec)
{
    (void)queue; (void)sensor; (void)usec;
    /* We generate one sample per frame regardless of the requested rate.
     * The engine low-pass filters its own input, so oversampling buys nothing. */
    return 0;
}

int ASensorEventQueue_hasEvents(void *queue)
{
    (void)queue;
    return (g_enabled && g_have_event) ? 1 : 0;
}

/* Deliver at most one sample per call. The engine drains in a loop, so
 * returning the same sample repeatedly would spin forever. */
ssize_t ASensorEventQueue_getEvents(void *queue, void *events_, size_t count)
{
    ASensorEvent *events = (ASensorEvent *)events_;
    (void)queue;
    if (!g_enabled || !g_have_event || !events || count == 0) return 0;

    memset(&events[0], 0, sizeof(ASensorEvent));
    events[0].version   = (int32_t)sizeof(ASensorEvent);
    events[0].sensor    = 0;
    events[0].type      = ASENSOR_TYPE_ACCELEROMETER;
    events[0].timestamp = (int64_t)armTicksToNs(armGetSystemTick());
    events[0].u.acceleration.x = g_x;
    events[0].u.acceleration.y = g_y;
    events[0].u.acceleration.z = g_z;

    g_have_event = 0;
    g_reports_since_drain = 0;      /* the game is collecting; all is well */
    sj_n_sensor_samples++;
    {
        static int n;
        if (n < 3) {
            printf("sj: accel sample %d = (%.2f, %.2f, %.2f) m/s^2\n",
                   n, g_x, g_y, g_z);
            n++;
        }
    }
    return 1;
}

/* --- ASensor property accessors ------------------------------------------- */
const char *ASensor_getName(const void *s)   { (void)s; return "Switch Tilt"; }
const char *ASensor_getVendor(const void *s) { (void)s; return "sonicjumpfever_nx"; }
int   ASensor_getType(const void *s)         { (void)s; return ASENSOR_TYPE_ACCELEROMETER; }
float ASensor_getResolution(const void *s)   { (void)s; return GRAVITY / 1024.0f; }
int   ASensor_getMinDelay(const void *s)     { (void)s; return 16667; }  /* 60 Hz */

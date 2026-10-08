/* sj_sensor.h -- accelerometer synthesis for tilt steering. MIT, see LICENSE. */
#ifndef SJ_SENSOR_H
#define SJ_SENSOR_H
#include <switch.h>
#define TILT_STICK 0
#define TILT_GYRO  1
#define TILT_BOTH  2
void sj_sensor_update(void);
void sj_sensor_get(float *x, float *y, float *z);
void sj_sensor_set_mode(int mode);
/* Treat the controller's current pose as neutral. */
void sj_sensor_recalibrate(void);

/* ALooper_pollOnce must report this ident when a sample is waiting: the game
 * only calls ASensorEventQueue_getEvents when it sees it. */
int   sj_sensor_pending(void);
int   sj_sensor_looper_ident(void);
void *sj_sensor_looper_data(void);
int  sj_sensor_get_mode(void);
/* Provided by main.c: the shared pad and a six-axis read that copes with
 * handheld / split / dual Joy-Con layouts. */
PadState *sj_pad(void);
int sj_read_sixaxis(HidSixAxisSensorState *out);
#endif

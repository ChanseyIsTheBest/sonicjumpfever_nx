/* sj_trace.h -- runtime counters and watchdog heartbeat. MIT, see LICENSE.
 *
 * Cheap, advisory counters bumped from the shim wrappers, plus a thread that
 * prints them periodically. Their whole purpose is to make a hang legible:
 * a crash leaves a report, a hang leaves nothing but a log that stops.
 */
#ifndef SJ_TRACE_H
#define SJ_TRACE_H

extern unsigned sj_n_swaps;      /* eglSwapBuffers      */
extern unsigned sj_n_polls;      /* ALooper_pollOnce    */
extern unsigned sj_n_jni;        /* routed JNI calls    */
extern unsigned sj_n_opens;      /* fopen/open attempts */
extern unsigned sj_n_open_fail;
extern unsigned sj_n_threads;    /* pthread_create      */
extern unsigned sj_n_cond_waits; /* pthread_cond_wait   */
extern unsigned sj_n_touches;    /* touch events injected */
extern unsigned sj_n_sensor_samples; /* tilt samples the game collected */
extern unsigned sj_n_audio_cb;     /* SDL audio callback invocations */
extern unsigned sj_n_enqueued;     /* SFX buffers enqueued by the engine */
extern unsigned sj_audio_peak;     /* peak |sample| of the last mixed buffer */
extern unsigned sj_n_voices_live;  /* voices in PLAYING state */
extern unsigned sj_n_music_frames; /* frames pulled from the music decoder */
extern double   sj_last_clock;   /* newest gettimeofday, in seconds */

/* Record that the calling thread just entered `what`. `what` must be a string
 * literal or otherwise outlive the process; nothing is copied. */
void sj_mark(const char *what);

void sj_trace_open(const char *path, int ok);
void sj_trace_jni(const char *what);
/* Start the watchdog thread; `verbose` also prints the 3 s heartbeat lines. */
void sj_trace_start(int verbose);
void sj_trace_stop(void);

/* Watchdog inputs (sj_trace.c). The main loop beats every iteration; focus
 * changes reset the render timer; disarm before an orderly shutdown. */
void sj_wd_main_beat(void);
void sj_wd_set_focus(int focused);
void sj_wd_disarm(void);

#endif

/* sj_threads.h -- where threads run, and at what priority. MIT, see LICENSE.
 *
 * Follows the pattern of the other wrappers in this tree (fruitninja_nx,
 * pvzultimate, sonicolympics_nx), each of which learned it on hardware:
 *
 *  - Horizon is strictly priority ordered and does not time-slice threads at
 *    normal priorities: a thread that busy-waits keeps its core from everything
 *    at its priority or below.
 *  - libnx's pthread_create makes threads at priority 0x3B with the ideal core
 *    set to the process default core. The engine's threads therefore all start
 *    piled onto core 0. They are spread round-robin over the allowed cores,
 *    with the affinity mask left covering all of them so the kernel can still
 *    move a thread off a busy core (fruitninja_nx "round 76").
 *  - Our own helper threads made with threadCreate(..., -2) are pinned to the
 *    default core outright. They are placed explicitly, falling back to the
 *    default core when applet mode refuses the one asked for (bananakong_nx).
 *  - hbloader only permits a narrow priority band around 0x2C, so a priority
 *    is tried from a list rather than assumed (sonicolympics_nx: 0x18 was
 *    refused outright and the watchdog never started).
 */
#ifndef SJ_THREADS_H
#define SJ_THREADS_H

#include <stddef.h>
#include <switch.h>

/* Call once from main(), on the main thread, before the engine starts. */
void sj_threads_init(void);

/* Spread the calling engine thread: next ideal core round-robin, affinity
 * left at every allowed core. `what` is logged. */
void sj_threads_spread_self(const char *what);

/* Create (not start) a helper thread, trying each priority in `prios` in turn
 * on `core` and then on the default core. Returns the priority used, or -1. */
int sj_threads_create(Thread *t, ThreadFunc fn, void *arg, size_t stack,
                      const int *prios, int nprios, int core, const char *what);

/* A core other than the main thread's for helpers, or -2 if there is none. */
int sj_threads_aux_core(void);

/* The main thread's priority, as Horizon reports it (often 0x2C, not always). */
int sj_threads_main_prio(void);

#endif

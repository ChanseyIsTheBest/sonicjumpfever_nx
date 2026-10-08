/* sj_threads.c -- spread threads over the allowed cores. MIT, see LICENSE.
 *
 * See sj_threads.h for the reasoning and where each rule came from.
 */

#include <stdio.h>
#include <switch.h>

#include "sj_threads.h"

static u64 g_mask;              /* cores this process may use */
static int g_cores[4];          /* allowed cores, the main thread's last */
static int g_ncores;
static int g_main_core = 0;
static int g_main_prio = 0x2C;
static int g_rr;                /* round-robin cursor for engine threads */

void sj_threads_init(void)
{
    u64 mask = 0;
    s32 prio = 0x2C;
    int c;

    g_main_core = (int)svcGetCurrentProcessorNumber();
    if (R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)))
        g_main_prio = prio;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !mask)
        mask = 1ULL << g_main_core;
    g_mask = mask;

    /* Other cores first, so the first engine thread (android_app_entry: the
     * game and all its rendering) lands away from our main loop. */
    g_ncores = 0;
    for (c = 0; c < 4; c++)
        if ((mask & (1ULL << c)) && c != g_main_core) g_cores[g_ncores++] = c;
    if (mask & (1ULL << g_main_core)) g_cores[g_ncores++] = g_main_core;

    printf("sj: threads: core mask 0x%llx (%d usable), main thread on core %d "
           "at priority 0x%x\n", (unsigned long long)mask, g_ncores,
           g_main_core, g_main_prio);
}

int sj_threads_main_prio(void) { return g_main_prio; }

int sj_threads_aux_core(void)
{
    /* The last of the non-main cores: core 2 in the usual 0/1/2 layout, leaving
     * core 1 to the game's render thread. */
    int i;
    for (i = g_ncores - 1; i >= 0; i--)
        if (g_cores[i] != g_main_core) return g_cores[i];
    return -2;
}

void sj_threads_spread_self(const char *what)
{
    int c;
    Result rc;
    s32 prio = 0;

    if (g_ncores <= 1) {
        printf("sj: %s started (one usable core; not spread)\n", what);
        return;
    }
    /* A racing increment costs at worst two threads sharing an ideal core,
     * which the full affinity mask lets the kernel correct. Deliberately not
     * an atomic builtin: those can compile to out-of-line libgcc helpers. */
    c = g_cores[(unsigned)(g_rr++) % (unsigned)g_ncores];
    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, (u32)g_mask);
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    printf("sj: %s -> ideal core %d, mask 0x%x, priority 0x%x%s\n", what, c,
           (unsigned)g_mask, (unsigned)prio, R_FAILED(rc) ? " (move REFUSED)" : "");
}

int sj_threads_create(Thread *t, ThreadFunc fn, void *arg, size_t stack,
                      const int *prios, int nprios, int core, const char *what)
{
    int i;
    for (i = 0; i < nprios; i++) {
        Result rc = 1;
        int used = core;
        if (core >= 0) rc = threadCreate(t, fn, arg, NULL, stack, prios[i], core);
        if (R_FAILED(rc)) {
            used = -2;          /* applet mode can refuse an explicit core */
            rc = threadCreate(t, fn, arg, NULL, stack, prios[i], -2);
        }
        if (R_SUCCEEDED(rc)) {
            printf("sj: %s thread: priority 0x%x, core %s%d\n", what, prios[i],
                   used < 0 ? "default " : "", used < 0 ? g_main_core : used);
            return prios[i];
        }
    }
    printf("sj: could not create the %s thread\n", what);
    return -1;
}

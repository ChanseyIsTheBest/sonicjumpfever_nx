/* sj_log.c -- crash-safe logging.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 * Modelled on the happywheels wrapper's util.c, (C) 2021 fgsfds.
 *
 * WHY THIS EXISTS
 * ---------------
 * A homebrew NRO that never calls consoleInit() has no console. libnx still
 * accepts printf and routes it through its software console renderer, which
 * dereferences a framebuffer that was never allocated -- a Data Abort at
 * address 0 inside ConsoleSwRenderer_drawChar, with a stack that points at
 * whatever happened to log last rather than at any real bug.
 *
 * That is a miserable failure mode: a diagnostic printf becomes the crash. It
 * is worse than useless during bring-up, when the whole point of the message
 * is to tell you what went wrong.
 *
 * So: no console. Everything goes to a file next to the game, and printf
 * itself is redirected there too (see sj_log_init) so that a stray printf --
 * in this port, in the inherited shims, or inside a portlib -- cannot bring
 * the process down.
 *
 * Writes are buffered and coalesced, because one write() syscall per line to
 * the SD card is slow enough to change the timing of what you are debugging.
 * sj_log_note() bypasses the buffer for events that tend to be followed by a
 * crash, where a buffered line would be lost.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <switch.h>
#include <sys/iosupport.h>   /* devoptab_t, devoptab_list, STD_OUT */

#include "sj_log.h"
#include "sj_paths.h"
#include "config.h"

#define LOG_BUF_SIZE 8192

static Mutex  g_lock;          /* libnx Mutex: zero-initialised == unlocked */
static int    g_fd = -1;
static char   g_buf[LOG_BUF_SIZE];
static int    g_used;
static int    g_enabled = 1;
static char   g_path[600];

static void flush_locked(void)
{
    int off = 0;
    if (!g_used) return;
    while (off < g_used) {
        ssize_t w = write(g_fd, g_buf + off, (size_t)(g_used - off));
        if (w <= 0) break;
        off += (int)w;
    }
    g_used = 0;
    /* Commit to the card, every time. write() alone leaves the data in the FS
     * service's cache, and a console that locks up hard never flushes it: the
     * first Fever log on hardware lost its last seconds -- exactly the part
     * that mattered -- and ended in stale sectors from some other program's
     * old log. This only runs with debug_log=1; the cost is a few ms a line. */
    fsync(g_fd);
}

/* devoptab hook: everything written to stdout/stderr lands here instead of the
 * console renderer. This is what makes a stray printf harmless. */
static ssize_t log_dev_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    if (!ptr || len == 0) return 0;
    sj_log_write(ptr, len);
    return (ssize_t)len;
}

static const devoptab_t g_log_devoptab = {
    .name    = "con",          /* replaces the console device */
    .write_r = log_dev_write,
};

/* The console device as it stood before the redirect. error.c writes the
 * status and crash screens through this: those two places really do own a
 * console (they call consoleInit first), and routing them through printf would
 * put the crash message in the log file and leave the player looking at a
 * blank screen.
 *
 * Read live rather than cached, because consoleInit/consoleExit reinstall the
 * device: fatal_error calls consoleInit again after startup_status_end has
 * called consoleExit, so a pointer captured once at startup would be stale by
 * the time the message that matters most is printed. */
static const devoptab_t *g_console_dev;

ssize_t con_write_direct(const char *data, size_t len)
{
    const devoptab_t *dev = devoptab_list[STD_OUT];

    if (!data || !len) return -1;
    /* If STD_OUT is still ours, the console was (re)installed elsewhere or
     * never existed; fall back to whatever we saved at init. */
    if (dev == &g_log_devoptab) dev = g_console_dev;
    if (!dev || !dev->write_r) return -1;
    return dev->write_r(NULL, NULL, data, len);
}

/* consoleInit() repoints STD_OUT at the console. Call this straight after to
 * put the log back in charge while keeping the new console reachable. */
void sj_log_recapture_console(void)
{
    const devoptab_t *dev = devoptab_list[STD_OUT];
    if (dev && dev != &g_log_devoptab) {
        g_console_dev = dev;
        devoptab_list[STD_OUT] = &g_log_devoptab;
        devoptab_list[STD_ERR] = &g_log_devoptab;
    }
}

void sj_log_init(void)
{
    mutexInit(&g_lock);
    snprintf(g_path, sizeof(g_path), "%s/" LOG_NAME, sj_home());

    /* Truncate on start: a log that grows across runs is hard to read and
     * eventually fills the card. */
    mutexLock(&g_lock);
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    mutexUnlock(&g_lock);

    /* Redirect stdout and stderr. devoptab_list[STD_OUT] normally points at
     * libnx's console device; pointing it here means printf, puts, perror and
     * anything inside a portlib writes to the file instead of a framebuffer
     * that does not exist. */
    /* Only capture a real console. startup_status_begin() runs before this and
     * already calls sj_log_recapture_console(), so STD_OUT may already be
     * ours; taking it unconditionally would set g_console_dev to the log
     * device itself and send the status and crash screens into the log file
     * instead of the display. */
    if (devoptab_list[STD_OUT] != &g_log_devoptab)
        g_console_dev = devoptab_list[STD_OUT];
    devoptab_list[STD_OUT] = &g_log_devoptab;
    devoptab_list[STD_ERR] = &g_log_devoptab;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    sj_log_note("=== sonicjumpfever_nx log ===\n");
}

void sj_log_write(const char *data, size_t len)
{
    if (!g_enabled || !data || len == 0) return;

    mutexLock(&g_lock);
    if (g_fd < 0) g_fd = open(g_path[0] ? g_path : LOG_NAME,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd >= 0) {
        if (g_used + (int)len > LOG_BUF_SIZE) flush_locked();
        if ((int)len > LOG_BUF_SIZE) {
            size_t off = 0;
            while (off < len) {
                ssize_t w = write(g_fd, data + off, len - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
        } else {
            memcpy(g_buf + g_used, data, len);
            g_used += (int)len;
        }
        /* Line-buffered, not block-buffered.
         *
         * The first version coalesced until the buffer filled, which is right
         * for a shipping game and useless for bring-up: the process died with
         * 8 KB of diagnostics still in RAM and the log on disk contained only
         * the header line. The whole point of the log is to survive the crash
         * it is describing.
         *
         * One write() per line costs a syscall, but the SD card write is
         * asynchronous and the lines are few. Set SJ_LOG_BLOCK_BUFFERED once
         * the port is stable and log volume actually matters. */
#ifndef SJ_LOG_BLOCK_BUFFERED
        if (memchr(data, '\n', len)) flush_locked();
#endif
    }
    mutexUnlock(&g_lock);
}

void sj_log_set_enabled(int on)
{
    if (on) { g_enabled = 1; return; }

    /* Switching off after the fact, since config.txt is only read once the log
     * is already running. Truncate rather than delete: a stale full log left
     * behind would be read as current, and an empty file is confusing on its
     * own, so leave one line saying why it is empty.
     *
     * stdout stays redirected here. It must: with no console, libnx routes
     * printf through a software renderer over a framebuffer that was never
     * allocated, so a stray printf would fault. Output is swallowed, not
     * handed back to the console. */
    mutexLock(&g_lock);
    g_used = 0;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    {
        int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            static const char msg[] =
                "logging is off (debug_log=0 in config.txt; set it to 1 to "
                "turn it back on)\n";
            ssize_t rc = write(fd, msg, sizeof(msg) - 1);
            (void)rc;
            close(fd);
        }
    }
    g_enabled = 0;
    mutexUnlock(&g_lock);
}

int sj_log_printf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    if (!g_enabled) return 0;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    sj_log_write(line, (size_t)n);
    return n;
}

int sj_log_note(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return n;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    sj_log_write(line, (size_t)n);
    sj_log_flush();          /* committed now: this is usually followed by a crash */
    return n;
}

void sj_log_flush(void)
{
    mutexLock(&g_lock);
    if (g_fd >= 0) flush_locked();
    mutexUnlock(&g_lock);
}

void sj_log_exit(void)
{
    mutexLock(&g_lock);
    if (g_fd >= 0) {
        flush_locked();
        fsync(g_fd);
        close(g_fd);
        g_fd = -1;
    }
    mutexUnlock(&g_lock);
}

const char *sj_log_path(void) { return g_path; }

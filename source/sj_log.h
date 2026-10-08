/* sj_log.h -- crash-safe logging. MIT, see LICENSE.
 *
 * Never call consoleInit() in this port. With no console, libnx's printf path
 * writes through a null framebuffer and faults inside its software renderer;
 * sj_log_init() redirects stdout/stderr to a file so that cannot happen.
 */
#ifndef SJ_LOG_H
#define SJ_LOG_H

#include <stddef.h>
#include <sys/types.h>

/* Call once, as early as possible after sj_paths_init(). */
void sj_log_init(void);

int  sj_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Written through immediately -- for events usually followed by a crash. */
int  sj_log_note(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));

void sj_log_write(const char *data, size_t len);
/* Turn the log off (or back on). Output is swallowed, never returned to the
 * console -- there is no console, and printf into one that does not exist
 * faults inside libnx's software renderer. */
void sj_log_set_enabled(int on);

void sj_log_flush(void);
void sj_log_exit(void);
const char *sj_log_path(void);

/* Write straight to the real console device, bypassing the log redirect.
 * Only for error.c's status and crash screens, which call consoleInit first.
 * Returns bytes written, or -1 if there is no console. */
ssize_t con_write_direct(const char *data, size_t len);

/* Call immediately after any consoleInit(): it repoints STD_OUT at the
 * console, and this hands stdout back to the log while keeping the console
 * reachable through con_write_direct. */
void sj_log_recapture_console(void);

#endif

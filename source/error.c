/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "error.h"
#include "sj_log.h"
#include <string.h>

static int status_active;

/* Screen output must not go through printf.
 *
 * sj_log_init() repoints devoptab_list[STD_OUT] at the log file so that a
 * stray printf anywhere in the port cannot fault on libnx's console renderer.
 * That redirect is unconditional, so the status and error screens -- the two
 * places that genuinely do own a console -- have to write to it directly
 * instead. Using printf here would put the crash message in the log file and
 * leave the player looking at a blank screen. */
static void con_puts(const char *s) {
  size_t n = strlen(s);
  while (n) {
    ssize_t w = con_write_direct(s, n);
    if (w <= 0) break;
    s += w;
    n -= (size_t)w;
  }
}

static void con_printf(const char *fmt, ...) {
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  con_puts(line);
}

void startup_status_update(const char *message) {
  if (!status_active) return;
  con_printf("\x1b[2J\x1b[H\n\n  Sonic Jump Fever NX\n\n  %s\n\n  Please wait...",
             message);
  consoleUpdate(NULL);
}

void startup_status_begin(const char *message) {
  if (!status_active) {
    consoleInit(NULL);
    sj_log_recapture_console();
    status_active = 1;
  }
  startup_status_update(message);
}

void startup_status_end(void) {
  if (!status_active) return;
  consoleExit(NULL);
  status_active = 0;
}

void fatal_error(const char *fmt, ...) {
  /* Commit whatever is buffered before we take over the screen: the log
   * usually explains what led here. */
  sj_log_flush();
  char message[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof message, fmt, ap);
  va_end(ap);
  if (status_active) startup_status_end();
  consoleInit(NULL);
  sj_log_recapture_console();
  con_printf("\x1b[2J\x1b[H\n\n  Sonic Jump Fever NX\n\n"
         "  Fatal error:\n\n  %s\n\n"
         "  Press + to exit.\n", message);
  consoleUpdate(NULL);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
    svcSleepThread(16000000ull);
  }
  consoleExit(NULL);
  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(1);
}

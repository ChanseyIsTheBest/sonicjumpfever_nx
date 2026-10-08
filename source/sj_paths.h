/* sj_paths.h -- runtime discovery of the game folder. MIT, see LICENSE.
 *
 * The folder can be named anything and live anywhere under /switch: the
 * loader finds itself from argv[0], or by scanning /switch one level deep.
 */
#ifndef SJ_PATHS_H
#define SJ_PATHS_H

#include <stddef.h>

/* Resolve the game folder. Call once, first thing in main(), before anything
 * touches the filesystem. Returns 0 on success, -1 if no folder containing
 * LIB_MAIN or a Sonic Jump Fever APK could be found. */
int sj_paths_init(int argc, char **argv);

/* The resolved folder, with no trailing slash, e.g. "sdmc:/switch/sonic".
 * Always returns a valid string: before sj_paths_init it is the compiled
 * default, so code that runs early cannot get NULL. */
const char *sj_home(void);

/* The assets directory: sj_home() + "/assets". */
const char *sj_assets(void);

/* sj_home() with a trailing slash, for the engine's internalDataPath: some of
 * its call sites concatenate the path and filename without a separator. */
const char *sj_home_slash(void);

/* Absolute path to the user's APK, which the engine opens as a zip archive.
 * game.apk if present, otherwise any .apk in the folder that contains the
 * arm64 libsonicjumpfever.so. Empty string if none was found -- the game
 * cannot initialise without it. */
const char *sj_apk(void);

/* Build "<home>/<rel>" into buf. Returns buf. rel may be NULL for the home
 * directory itself. */
char *sj_path(char *buf, size_t cap, const char *rel);

/* How the folder was found -- shown once at startup so a user who put the
 * files somewhere unexpected can see what happened. */
const char *sj_paths_origin(void);

#endif

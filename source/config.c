/* config.c -- tiny key=value config reader. MIT licensed, see LICENSE. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "config.h"
#include "sj_sensor.h"

Config config = { CONFIG_VERSION, "auto", TILT_BOTH, 0, 1, 100, 1,
                  /* back_button */ 1, /* quit_button */ 0,
                  /* clean_exit */ 0, /* rotation */ 0,
                  /* debug_log */ 0, /* gyro */ 1,
                  /* stick_sens */ 100, /* watchdog */ 1 };

/* Accepted language codes: the ISO codes Fever's getSystemLanguage compares
 * against. "pt-br" is accepted as a friendly alias and normalised to "pt",
 * because the file the user sees in the APK is strings_pt-br.loc. */
static const char *g_langs[] = {
  "auto", "en", "fr", "de", "it", "es", "pt", "ru", NULL
};

int sj_language_is_valid(const char *code) {
  int i;
  for (i = 0; g_langs[i]; i++)
    if (!strcasecmp(code, g_langs[i])) return 1;
  return 0;
}

int read_config(const char *file) {
  char line[128];
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    char key[64], sval[64]; int val;
    if (line[0] == '#' || line[0] == '\n') continue;

    /* language is the one string-valued setting. Parse it before the numeric
     * path, which would otherwise reject "language=en" outright. */
    if (sscanf(line, "%63[^= ] = %63s", key, sval) == 2 ||
        sscanf(line, "%63[^=]=%63s", key, sval) == 2) {
      char *c = strchr(sval, '#');          /* trailing comment */
      if (c) *c = '\0';
      if (!strcmp(key, "language")) {
        if (!strcasecmp(sval, "pt-br") || !strcasecmp(sval, "pt_br"))
          snprintf(sval, sizeof(sval), "pt");
        if (sj_language_is_valid(sval)) {
          snprintf(config.language, sizeof(config.language), "%s", sval);
        } else {
          /* Actually fall back, rather than only saying so: a code the game
           * does not recognise would silently resolve to English anyway. */
          printf("sj: config.txt: unknown language '%s', using auto\n", sval);
          snprintf(config.language, sizeof(config.language), "auto");
        }
        continue;
      }
    }

    if (sscanf(line, "%63[^= ] = %d", key, &val) != 2 &&
        sscanf(line, "%63[^=]=%d", key, &val) != 2) continue;
    if      (!strcmp(key, "config_version")) config.version     = val;
    else if (!strcmp(key, "rotation"))       config.rotation    = val;
    else if (!strcmp(key, "tilt_invert"))    config.tilt_invert = val;
    else if (!strcmp(key, "debug_log"))      config.debug_log   = val;
    else if (!strcmp(key, "gyro"))           config.gyro        = val;
    else if (!strcmp(key, "stick_sens"))     config.stick_sens  = val;
    else if (!strcmp(key, "back_button"))    config.back_button = val;
    else if (!strcmp(key, "watchdog"))       config.watchdog    = val;
    /* "store" was a test switch in builds 4-5; it is fixed off now
     * (SJ_STORE_ENABLED) and ignored if an older file still has it. */
  }
  fclose(f);

  if (config.version != CONFIG_VERSION) {
    printf("sj: config.txt written by a different build (v%d); rewriting as "
           "v%d\n", config.version, CONFIG_VERSION);
    /* v2 (1.0.0): the orange-screen test builds handed out config.txt files
     * with gyro=0 and debug_log=1. Put both back to the release defaults --
     * motion on, log off -- once; either can be changed again afterwards. */
    if (config.version < 2) {
      config.gyro      = 1;
      config.debug_log = 0;
    }
    config.version = CONFIG_VERSION;
    write_config(file);          /* persist the migration */
  }

  /* Fixed settings: pin them after parsing so a hand-edited config.txt cannot
   * leave a stale value behind. */
  config.show_cursor  = 1;
  config.music_volume = 100;
  config.quit_button  = 0;   /* Minus closed the game with no confirmation */
  config.clean_exit   = 0;   /* terminate rather than race the engine threads */
  config.decode_stream_audio = 1;

  if (!config.language[0] || !sj_language_is_valid(config.language))
    snprintf(config.language, sizeof(config.language), "auto");
  if (config.rotation < 0 || config.rotation > 2) config.rotation = 0;
  config.tilt_invert = config.tilt_invert ? 1 : 0;
  config.debug_log   = config.debug_log   ? 1 : 0;
  config.gyro        = config.gyro        ? 1 : 0;
  config.back_button = config.back_button ? 1 : 0;
  config.watchdog    = config.watchdog    ? 1 : 0;
  if (config.stick_sens < SJ_STICK_SENS_MIN) config.stick_sens = SJ_STICK_SENS_MIN;
  if (config.stick_sens > SJ_STICK_SENS_MAX) config.stick_sens = SJ_STICK_SENS_MAX;
  /* tilt_mode is derived, not configured: gyro on means stick AND motion. */
  config.tilt_mode   = config.gyro ? TILT_BOTH : TILT_STICK;
  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) return -1;

  /* One fprintf per setting, deliberately: a single fprintf with many
   * positional arguments is how a key ends up next to someone else's value,
   * and -Wformat cannot catch it when the count and types still match. */
  fprintf(f, "# sonicjumpfever_nx configuration\n#\n");

  fprintf(f, "# config_version lets a new build apply changed defaults to an\n"
             "# existing file. Leave it alone.\n"
             "config_version=%d\n\n", CONFIG_VERSION);

  fprintf(f, "# language: which localisation the game uses.\n"
             "#\n"
             "#   auto    follow the console's system language\n"
             "#   en      English\n"
             "#   fr      French\n"
             "#   de      German\n"
             "#   it      Italian\n"
             "#   es      Spanish\n"
             "#   pt      Portuguese (Brazil)  (pt-br is accepted too)\n"
             "#   ru      Russian\n"
             "#\n"
             "# Those seven are every localisation Sonic Jump Fever ships.\n"
             "# Any other console language (Japanese, Korean, Chinese, Dutch)\n"
             "# falls back to English, as the game itself does.\n"
             "language=%s\n\n", config.language);

  fprintf(f, "# back_button: B acts as the Android back key (0 or 1). It closes\n"
             "# popups and backs out of menus. It is ignored wherever Back\n"
             "# would reach the home screen, where the game would otherwise\n"
             "# quit with no warning. With 0, B is a jump button instead.\n"
             "back_button=%d\n\n", config.back_button);

  fprintf(f, "# tilt_invert: mirror the steering left/right (0 or 1).\n"
             "# Applies to both the stick and the controller's motion.\n"
             "# L + R + ZL + ZR held together flips it in game.\n"
             "tilt_invert=%d\n\n", config.tilt_invert);

  fprintf(f, "# gyro: let the controller's motion steer as well as the left\n"
             "# stick (0 or 1). Clicking the left stick toggles this in-game\n"
             "# and saves it here. With 0 the motion sensors are not started\n"
             "# at all until you turn it on.\n"
             "gyro=%d\n\n", config.gyro);

  fprintf(f, "# stick_sens: how far the left stick leans the character, as a\n"
             "# percent (%d-%d). D-pad left and right adjust this in game.\n"
             "stick_sens=%d\n\n",
             SJ_STICK_SENS_MIN, SJ_STICK_SENS_MAX, config.stick_sens);

  fprintf(f, "# rotation: 0 = none, 1 = display rotated 90 clockwise,\n"
             "# 2 = rotated 90 counter-clockwise (TATE / portrait output).\n"
             "# Taps and tilt both follow the picture.\n"
             "rotation=%d\n\n", config.rotation);

  fprintf(f, "# debug_log: write %s and run the heartbeat that\n"
             "# prints a status line every few seconds (0 or 1). Turn it on if\n"
             "# anything misbehaves and include the file when reporting it.\n"
             "debug_log=%d\n\n", LOG_NAME, config.debug_log);

  fprintf(f, "# watchdog: if the game freezes (no frames for 30 s, or the\n"
             "# loader's own loop stuck for 15 s), stop it deliberately so\n"
             "# Atmosphere saves a crash report with every thread's backtrace\n"
             "# in sdmc:/atmosphere/crash_reports/. 0 to disable.\n"
             "watchdog=%d\n\n", config.watchdog);

  fclose(f);
  return 0;
}

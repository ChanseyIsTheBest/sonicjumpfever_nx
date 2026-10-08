/* config.h -- sonicjumpfever_nx build and runtime configuration.
 * MIT licensed, see LICENSE. */
#ifndef SJ_CONFIG_H
#define SJ_CONFIG_H
#include <stddef.h>

#define LIB_MAIN          "libsonicjumpfever.so"
/* Where the library lives inside the APK. main.c extracts it from there on
 * first boot, so the user only has to copy the APK. */
#define LIB_APK_PATH      "lib/arm64-v8a/libsonicjumpfever.so"
#define GAME_PACKAGE      "com.sega.sonicjumpfever"
#define GAME_VERSION_NAME "1.6.1"
#define GAME_VERSION_CODE 161
#define CONFIG_NAME       "config.txt"
#define LOG_NAME          "sonicjumpfever_nx.log"

/* Save editing (sonicdash_nx's design): save_edit.txt in the game folder is
 * applied to the game's profile at every launch, and the profile re-signed
 * exactly as the game signs it; save_contents.txt lists what the save holds.
 * sj_saveedit.c. 0 = never touch the save. */
#define SJ_SAVE_EDIT        1
#define SAVE_NAME           "profile0.dat"
#define SAVE_EDIT_NAME      "save_edit.txt"
#define SAVE_CONTENTS_NAME  "save_contents.txt"

/* Bumped whenever a DEFAULT changes in a way that should reach users who
 * already have a config.txt on the card. */
#define CONFIG_VERSION   2

/* Left-stick steering range. 25% is still playable and 300% is past the point
 * the game clamps anyway, so the limits are generous rather than tuned. */
#define SJ_STICK_SENS_MIN  25
#define SJ_STICK_SENS_MAX  300
#define SJ_STICK_SENS_STEP 25
#define GAME_HOME         "sdmc:/switch/sonicjumpfever_nx"

extern int screen_width;
extern int screen_height;

/* Language codes.
 *
 * Sonic Jump Fever asks Loader.getLanguage() for a two-letter ISO-639-1 code
 * and compares it against exactly these: en, ru, fr, de, es, it, pt
 * (strings::getSystemLanguage, confirmed by disassembly). Anything else is
 * English. It ships strings_{de,en,es,fr,it,pt-br,ru}.loc -- note the
 * engine asks for "pt" and maps it to pt-br itself, unlike the original
 * Sonic Jump which wanted "pt-br" and "jp" passed in directly. There is no
 * Japanese, Korean or Chinese localisation in Fever. "auto" follows the
 * console. */
#define LANG_AUTO "auto"

typedef struct {
  int version;        /* CONFIG_VERSION the file was written with */
  char language[8];   /* "auto" or a code from the list above */

  /* Fixed, not exposed in config.txt. They stay as struct fields because the
   * shared code reads them; they are simply not parsed or written. */
  int tilt_mode;      /* TILT_STICK / TILT_GYRO / TILT_BOTH */
  int tilt_invert;    /* mirror steering left/right; in config.txt */
  int show_cursor;    /* docked pointer */
  int music_volume;   /* 0..100 */
  /* Allow opensles.c to decode a compressed OpenSL source in-port with
   * minimp3. Fever's SFX are PCM buffer queues, so this is a fallback. */
  int decode_stream_audio;
  /* B -> the game's Back handler (Loader.triggerBack). On by default: the
   * port suppresses it whenever it would reach the home screen, where Back
   * quits the game outright with no confirmation (HomeScreen::onBackButton
   * calls slRequestShutdown); see sj_jni_send_back. Everywhere else it closes
   * popups and backs out of menus, as the Android back key did. */
  int back_button;
  /* Map Minus to "quit the game". Off: it closed the game with no warning,
   * which reads as a crash. */
  int quit_button;
  /* 0 = terminate the process on quit (avoids racing engine threads that
   * outlive onDestroy); 1 = return from main normally. */
  int clean_exit;
  /* Screen rotation, for a rotated (TATE) display: 0 = none, 1 = render
   * rotated 90 CW, 2 = rotated 90 CCW. */
  int rotation;
  /* Write sonicjumpfever_nx.log and run the heartbeat. Off by default. */
  int debug_log;
  /* Let the controller's motion steer as well as the stick. Clicking the left
   * stick toggles this in-game. */
  int gyro;
  /* Steering sensitivity for the LEFT STICK, as a percentage. D-pad left and
   * right adjust it in game. */
  int stick_sens;
  /* Turn a hang into a crash report: if the main loop stops for 15 s, or the
   * game stops drawing for 30 s while in focus, break deliberately so
   * Atmosphere records every thread's backtrace (sj_trace.c). On by default:
   * a report is worth far more than a console frozen on the last frame. */
  int watchdog;
} Config;

/* The game's store (sl::store) stays switched off: checkBillingSupported is
 * answered "unsupported", so the store never enables, never lists products
 * and never loads or saves purchases.xml. With it on, launches ended in
 * Atmosphere's orange secure-monitor screen within 250 ms of "store enabled"
 * (three logs, two builds); with it off (the store=0 test, builds 4-5) the
 * game ran every time. Nothing can be bought on Switch anyway, and the two
 * permanent purchases are save values save_edit.txt sets. Not a config.txt
 * setting on purpose. */
#define SJ_STORE_ENABLED 0

extern Config config;

int sj_language_is_valid(const char *code);
int read_config(const char *file);
int write_config(const char *file);
#endif

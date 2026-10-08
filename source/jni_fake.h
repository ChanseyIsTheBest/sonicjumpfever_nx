/* JNI compatibility environment for Unity and the game libraries.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish
extern volatile int jni_quit_requested;

void jni_init(void);
void jni_process_soft_keyboard(void);

// the fake MyNativeActivity jobject handed to ANativeActivity.clazz
void *jni_make_activity_object(void);

/* Raw bytes behind a fake byte[]. Declared here because jni_fake.c calls it
 * ~600 lines above its definition: without a prototype the implicit int
 * declaration truncates the returned pointer to 32 bits on aarch64. */
void *jni_bytearray_data(void *arr, int *len_out);

/* The two-letter ISO-639-1 code Sonic Jump Fever's getSystemLanguage expects
 * (en, fr, de, it, es, pt, ru). Any other console language reports "en". */
const char *jni_game_lang_code(void);

/* ISO-3166 alpha-3 country matching the language ("USA", "FRA", ...), for
 * Loader.getLocaleISO3CountryCode(). */
const char *jni_locale_iso3_country(void);

// fake Java object / string constructors
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

// Stable Android-style locale name (for example "fr_FR") selected from libnx.
const char *jni_locale_name(void);

#endif

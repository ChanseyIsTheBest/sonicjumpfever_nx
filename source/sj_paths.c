/* sj_paths.c -- find the game folder at runtime. MIT, see LICENSE.
 *
 * Three strategies, in order of trustworthiness:
 *
 *   1. argv[0]. hbmenu passes the full path of the .nro it launched, e.g.
 *      "sdmc:/switch/sonicjumpfever/sonicjumpfever_nx.nro". Strip the
 *      filename and we have the folder, whatever the user called it.
 *
 *   2. Scan /switch for a directory holding the game. Covers being launched
 *      through a forwarder or as an installed title, where argv[0] is missing
 *      or meaningless. One level deep only -- deep-scanning a user's SD card
 *      is slow and would happily wander into a folder of ROMs.
 *
 *   3. The compiled default. Last resort so the error message can name a real
 *      path instead of nothing.
 *
 * A folder "holds the game" if it has the extracted libsonicjumpfever.so, or
 * an APK that actually contains lib/arm64-v8a/libsonicjumpfever.so -- the
 * loader extracts the library from the APK on first boot, so a fresh install
 * is just the .nro and the APK. Checking inside the APK rather than trusting
 * the extension means a folder with some other game's APK in it is not
 * mistaken for this one.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <strings.h>

#include "sj_paths.h"
#include "sj_apkzip.h"
#include "config.h"

static char g_apk[600];
static int  g_apk_searched;
static char g_home[512]   = GAME_HOME;
static char g_assets[512] = GAME_HOME "/assets";
static const char *g_origin = "compiled default";

const char *sj_home(void)        { return g_home; }
const char *sj_assets(void)      { return g_assets; }
const char *sj_paths_origin(void){ return g_origin; }

static int is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Is `path` an APK carrying the arm64 Fever library? */
static int is_fever_apk(const char *path)
{
    SjZipEntry e;
    return sj_zip_find(path, LIB_APK_PATH, &e) == 0;
}

/* Find the game's APK in `dir`. game.apk is preferred; otherwise the first
 * .apk that actually contains the Fever library, so the user can keep the
 * original filename (sonic-jump-fever-1-6-1.apk or whatever it is called).
 * Returns 1 and fills `out` if one was found. */
static int find_apk_in(const char *dir, char *out, size_t cap)
{
    char cand[600];
    DIR *d;
    struct dirent *e;
    int found = 0;

    snprintf(cand, sizeof(cand), "%s/game.apk", dir);
    if (is_file(cand) && is_fever_apk(cand)) {
        snprintf(out, cap, "%s", cand);
        return 1;
    }

    d = opendir(dir);
    if (!d) return 0;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n <= 4 || strcasecmp(e->d_name + n - 4, ".apk")) continue;
        snprintf(cand, sizeof(cand), "%s/%s", dir, e->d_name);
        if (is_fever_apk(cand)) {
            snprintf(out, cap, "%s", cand);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* The engine opens its own assets as a zip: appPreInitialise is gated on
 *     sl::ZipFile::openArchive(getApkFileName())
 * returning true, and if it fails the whole init chain -- createAudio,
 * createNetwork, slInitialise, appInitialise -- is skipped. So the APK itself
 * has to be on the card, not just its extracted contents. */
const char *sj_apk(void)
{
    if (!g_apk_searched) {
        g_apk_searched = 1;
        if (!find_apk_in(g_home, g_apk, sizeof(g_apk))) g_apk[0] = '\0';
    }
    return g_apk;      /* "" if nothing was found */
}

/* The same folder with a trailing slash.
 *
 * android_main builds its storage path as getFilesDir().getPath() + "/", and
 * the original Sonic Jump was seen concatenating a base path and a filename
 * with no separator at all. A trailing slash makes the second kind work;
 * fopen_fake (and the rename/remove wrappers in imports.c) collapse the double
 * slash the first kind then produces. */
const char *sj_home_slash(void)
{
    static char buf[520];
    if (!buf[0]) snprintf(buf, sizeof(buf), "%s/", g_home);
    return buf;
}

char *sj_path(char *buf, size_t cap, const char *rel)
{
    if (rel && *rel) snprintf(buf, cap, "%s/%s", g_home, rel);
    else             snprintf(buf, cap, "%s", g_home);
    return buf;
}

static int looks_like_game_dir(const char *dir)
{
    char probe[600];
    snprintf(probe, sizeof(probe), "%s/%s", dir, LIB_MAIN);
    if (is_file(probe)) return 1;
    return find_apk_in(dir, probe, sizeof(probe));
}

static void adopt(const char *dir, const char *origin)
{
    snprintf(g_home, sizeof(g_home), "%s", dir);
    snprintf(g_assets, sizeof(g_assets), "%s/assets", g_home);
    g_origin = origin;
    g_apk_searched = 0;            /* re-resolve against the adopted folder */
}

/* Strip the trailing "/name.nro" from argv[0]. */
static int dir_of(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t n;
    if (!path || !*path) return -1;
    slash = strrchr(path, '/');
    if (!slash) return -1;
    n = (size_t)(slash - path);
    if (n == 0 || n >= cap) return -1;
    memcpy(out, path, n);
    out[n] = '\0';
    return 0;
}

int sj_paths_init(int argc, char **argv)
{
    char dir[512];

    /* 1. next to the .nro we were launched from */
    if (argc > 0 && argv && argv[0] && dir_of(argv[0], dir, sizeof(dir)) == 0) {
        if (looks_like_game_dir(dir)) {
            adopt(dir, "next to the .nro");
            return 0;
        }
    }

    /* 2. one level under /switch */
    {
        static const char *roots[] = { "sdmc:/switch", "/switch" };
        for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
            DIR *d = opendir(roots[r]);
            struct dirent *e;
            if (!d) continue;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                snprintf(dir, sizeof(dir), "%s/%s", roots[r], e->d_name);
                if (looks_like_game_dir(dir)) {
                    closedir(d);
                    adopt(dir, "found by scanning /switch");
                    return 0;
                }
            }
            closedir(d);
        }
    }

    /* 3. compiled default -- may not exist, but gives the error a real path */
    if (looks_like_game_dir(g_home)) {
        g_origin = "compiled default";
        return 0;
    }

    /* Nothing found. Still adopt the .nro's folder if we have one, so the
     * error message names the folder the user actually put things in. */
    if (argc > 0 && argv && argv[0] && dir_of(argv[0], dir, sizeof(dir)) == 0)
        adopt(dir, "next to the .nro (no game files found)");
    return -1;
}

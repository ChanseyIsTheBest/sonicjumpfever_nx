/* ---------------------------------------------------------------------------
 * sj_saveedit.c -- edit profile0.dat at boot from save_edit.txt (the
 * sonicdash_nx design, after battd_nx). MIT licensed, see LICENSE.
 *
 * THE SAVE (decoded from libsonicjumpfever.so 1.6.1 and verified on a real
 * profile0.dat: 3049 bytes, signature 0xdd1f9caf)
 * profile0.dat is one sl::DOMNode tree in the engine's binary form. Every value
 * is a type byte followed by its payload, little-endian:
 *     00 null      01 bool (1 byte)   02 int32    03 int64     04 float
 *     05 double    06 string (LEB128 length, bytes)
 *     07 object (LEB128 count; each entry = 1-byte key length, key, value)
 *     08 array  (LEB128 count, values)            09 DateTime (int64, ms)
 * The root holds localPlayerState (play statistics), synchronizedPlayerState
 * (rings, boosters, characters, rank...) and ftueStats.
 *
 * THE SIGNATURE (ConnectedProfileManager::loadPlayerState)
 * Two values hold it: localPlayerState.zoneState and ftueStats. To check it,
 * the game puts totalNumberOfPlays into zoneState and totalHeight into
 * ftueStats, then takes zlib's CRC-32 of the whole tree, starting from
 * 0x47155778 (sl::CRC32::update(DOMNode*)):
 *     null -> int32 0, bool -> its byte, numbers -> their little-endian bytes,
 *     string -> its bytes, array -> int32 count then every element,
 *     object -> int32 count then, in order of each key's FNV-1 32-bit hash,
 *               the hash and the value.
 * zoneState must equal that CRC, and ftueStats must equal
 * CRC * 0xBDC70E445308C215 (mod 2^64). Otherwise the game discards the save.
 * That's why editing profile0.dat by hand loses your progress, and why this
 * file exists.
 *
 * HOW IT EDITS
 * save_edit.txt is written once, with every setting commented out. An
 * uncommented setting is applied at EVERY launch, for as long as it stays
 * uncommented. Only values the save already holds are changed, in place and
 * keeping their type. Nothing is ever added or removed.
 * save_contents.txt lists everything in the save, so the binary file can be
 * read without tools. It starts with what the editor did at the last launch,
 * because the port's log is off by default.
 *
 * SAFETY, in the order it happens (sonicdash_nx's sequence)
 *   1. Parse profile0.dat. Stop if the layout is wrong, or if the signature
 *      does not match (unless fix_hand_edited_save is set and the layout is
 *      intact).
 *   2. Apply only the uncommented settings. If nothing differs, stop without
 *      writing.
 *   3. Re-sign and serialize, then PARSE THE RESULT: the signature must
 *      verify, and the tree must equal the edited one value for value.
 *   4. Keep the untouched original once, as profile0.dat.orig.
 *   5. Write profile0.dat.tmp, rename it over profile0.dat, then commit the
 *      SD card.
 * Portable C: the core compiles and is tested on a PC against a real save
 * (tools/test_saveedit.c).
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include "sj_saveedit.h"

#ifdef __SWITCH__
#include <switch.h>
#include "config.h"
#include "sj_paths.h"
#endif

#define SE_MAX_FILE   (4u << 20)
#define SE_MAX_DEPTH  64
#define SE_PATH_CAP   512
#define SIG_SEED      0x47155778u
#define SIG_MUL       0xBDC70E445308C215ull

/* synchronizedPlayerState.awardFlags (PlayerProfile::Flag) */
#define FLAG_REFILL_REDUCER (1u << 14)  /* hasRefillReducer() = isFlagSet(14) */
#define FLAG_RANK_FORMAT    (1u << 20)  /* score is (rank << 16) | progress;
                                           recalculateTotalScore converts it once */
/* ranks.xml holds 51 thresholds, the first one commented "Rank 2". So the
 * stored rank runs from 0 to 51 (addTotalScore stops at getMaxRank()), and the
 * game shows one more than it stores. */
#define FEVER_MAX_RANK 52

/* ------------------------------------------------------------ logging
 * Each line goes to the port's log (stdout is redirected there on Switch) and
 * into a report that heads save_contents.txt, because the log is off unless
 * debug_log = 1. */
static char   g_report[8192];
static size_t g_rlen;
static int    g_wrote;           /* something on the SD card changed */

static void se_log(const char *fmt, ...) {
  char line[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fputs(line, stdout);
  const size_t l = strlen(line);
  if (g_rlen + l < sizeof g_report) { memcpy(g_report + g_rlen, line, l + 1); g_rlen += l; }
}
#define SE_LOG(...) se_log(__VA_ARGS__)

/* ------------------------------------------------------------ small helpers */
static int ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return *a == *b;
}
static int ieqn(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    if (!a[i]) return 1;
  }
  return 1;
}
static char *trim(char *p) {
  while (isspace((unsigned char)*p)) p++;
  char *e = p + strlen(p);
  while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
  return p;
}
static int is_ascii_line(const char *v) {
  for (; *v; v++) if ((unsigned char)*v < 0x20 || (unsigned char)*v > 0x7E) return 0;
  return 1;
}
/* true/false, 1/0, yes/no, on/off; -1 if none of them */
static int parse_bool(const char *v) {
  if (ieq(v, "true") || !strcmp(v, "1") || ieq(v, "yes") || ieq(v, "on")) return 1;
  if (ieq(v, "false") || !strcmp(v, "0") || ieq(v, "no") || ieq(v, "off")) return 0;
  return -1;
}
/* A whole number, optionally negative; 0 if v is anything else. */
static int parse_ll(const char *v, long long *out) {
  const char *d = v + (*v == '-');
  if (!*d || strlen(d) > 19) return 0;
  for (const char *p = d; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
  errno = 0;
  const long long n = strtoll(v, NULL, 10);
  if (errno == ERANGE) return 0;
  *out = n;
  return 1;
}
/* A whole number from 0 to max. */
static int parse_count(const char *v, long long max, long long *out) {
  long long n;
  if (*v == '-' || !parse_ll(v, &n) || n > max) return 0;
  *out = n;
  return 1;
}

/* ------------------------------------------------------------ the tree */
enum { T_NULL, T_BOOL, T_I32, T_I64, T_FLOAT, T_DOUBLE, T_STR, T_OBJ, T_ARR, T_TIME };

typedef struct Node {
  uint8_t t;
  uint64_t raw;              /* bool, numbers, DateTime: the stored bytes, little-endian */
  char *s; size_t sn;        /* T_STR: the bytes */
  char *key; size_t kn;      /* this value's key inside its object, else NULL */
  struct Node *kid; int n;   /* T_OBJ / T_ARR: members in file order */
} Node;

static void node_free(Node *x) {
  if (!x) return;
  for (int i = 0; i < x->n; i++) node_free(&x->kid[i]);
  free(x->kid); free(x->s); free(x->key);
  memset(x, 0, sizeof *x);
}

static Node *child(Node *o, const char *key) {
  if (!o || o->t != T_OBJ) return NULL;
  for (int i = 0; i < o->n; i++)
    if (o->kid[i].key && !strcmp(o->kid[i].key, key)) return &o->kid[i];
  return NULL;
}

static int32_t  as_i32(const Node *x) { return (int32_t)(uint32_t)x->raw; }

/* ------------------------------------------------------------ reading */
typedef struct { const uint8_t *b; size_t len, o; char *err; size_t errsz; } Rd;

static int rd_fail(Rd *r, const char *what) {
  snprintf(r->err, r->errsz, "%s at byte %lu", what, (unsigned long)r->o);
  return -1;
}
static int rd_leb(Rd *r, uint32_t *v) {
  uint32_t x = 0;
  for (int sh = 0; sh < 35; sh += 7) {
    if (r->o >= r->len) return rd_fail(r, "cut short");
    const uint8_t c = r->b[r->o++];
    x |= (uint32_t)(c & 0x7F) << sh;
    if (!(c & 0x80)) { *v = x; return 0; }
  }
  return rd_fail(r, "bad length");
}
static int rd_le(Rd *r, int bytes, uint64_t *v) {
  if (r->len - r->o < (size_t)bytes) return rd_fail(r, "cut short");
  uint64_t x = 0;
  for (int i = 0; i < bytes; i++) x |= (uint64_t)r->b[r->o + i] << (8 * i);
  r->o += (size_t)bytes;
  *v = x;
  return 0;
}
static int rd_node(Rd *r, Node *x, int depth) {
  if (depth > SE_MAX_DEPTH) return rd_fail(r, "nested too deeply");
  if (r->o >= r->len) return rd_fail(r, "cut short");
  const uint8_t t = r->b[r->o++];
  x->t = t;
  switch (t) {
  case T_NULL:   return 0;
  case T_BOOL:   return rd_le(r, 1, &x->raw);
  case T_I32:
  case T_FLOAT:  return rd_le(r, 4, &x->raw);
  case T_I64:
  case T_DOUBLE:
  case T_TIME:   return rd_le(r, 8, &x->raw);
  case T_STR: {
    uint32_t n;
    if (rd_leb(r, &n)) return -1;
    if (r->len - r->o < n) return rd_fail(r, "text cut short");
    if (!(x->s = malloc((size_t)n + 1))) return rd_fail(r, "out of memory");
    memcpy(x->s, r->b + r->o, n); x->s[n] = 0; x->sn = n; r->o += n;
    return 0;
  }
  case T_OBJ:
  case T_ARR: {
    uint32_t n;
    if (rd_leb(r, &n)) return -1;
    if (n > r->len - r->o) return rd_fail(r, "bad member count");   /* each is >= 1 byte */
    if (!(x->kid = calloc(n ? n : 1, sizeof *x->kid))) return rd_fail(r, "out of memory");
    x->n = (int)n;                                    /* zeroed: node_free is safe midway */
    for (uint32_t i = 0; i < n; i++) {
      Node *k = &x->kid[i];
      if (t == T_OBJ) {
        if (r->o >= r->len) return rd_fail(r, "cut short");
        const uint8_t kl = r->b[r->o++];
        if (r->len - r->o < kl) return rd_fail(r, "name cut short");
        if (!(k->key = malloc((size_t)kl + 1))) return rd_fail(r, "out of memory");
        memcpy(k->key, r->b + r->o, kl); k->key[kl] = 0; k->kn = kl; r->o += kl;
      }
      if (rd_node(r, k, depth + 1)) return -1;
    }
    return 0;
  }
  }
  r->o--;
  return rd_fail(r, "unknown value type");
}

/* ------------------------------------------------------------ writing */
typedef struct { uint8_t *b; size_t n, cap; int oom; } Wr;

static void w_bytes(Wr *w, const void *p, size_t n) {
  if (w->oom) return;
  if (w->n + n > w->cap) {
    size_t c = w->cap ? w->cap : 4096;
    while (c < w->n + n) c *= 2;
    uint8_t *nb = realloc(w->b, c);
    if (!nb) { w->oom = 1; return; }
    w->b = nb; w->cap = c;
  }
  memcpy(w->b + w->n, p, n);
  w->n += n;
}
static void w_byte(Wr *w, uint8_t c) { w_bytes(w, &c, 1); }
static void w_le(Wr *w, uint64_t v, int bytes) {
  uint8_t b[8];
  for (int i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i));
  w_bytes(w, b, (size_t)bytes);
}
static void w_leb(Wr *w, uint32_t v) {
  do { uint8_t c = v & 0x7F; v >>= 7; if (v) c |= 0x80; w_byte(w, c); } while (v);
}
static void w_printf(Wr *w, const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n > 0) w_bytes(w, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}
static void w_node(Wr *w, const Node *x) {
  w_byte(w, x->t);
  switch (x->t) {
  case T_NULL:  break;
  case T_BOOL:  w_le(w, x->raw, 1); break;
  case T_I32:
  case T_FLOAT: w_le(w, x->raw, 4); break;
  case T_STR:   w_leb(w, (uint32_t)x->sn); w_bytes(w, x->s, x->sn); break;
  case T_OBJ:
  case T_ARR:
    w_leb(w, (uint32_t)x->n);
    for (int i = 0; i < x->n; i++) {
      if (x->t == T_OBJ) { w_byte(w, (uint8_t)x->kid[i].kn); w_bytes(w, x->kid[i].key, x->kid[i].kn); }
      w_node(w, &x->kid[i]);
    }
    break;
  default:      w_le(w, x->raw, 8); break;      /* int64, double, DateTime */
  }
}

static int node_eq(const Node *a, const Node *b) {
  if (a->t != b->t || a->raw != b->raw || a->n != b->n || a->sn != b->sn || a->kn != b->kn) return 0;
  if (a->sn && memcmp(a->s, b->s, a->sn)) return 0;
  if (a->kn && memcmp(a->key, b->key, a->kn)) return 0;
  for (int i = 0; i < a->n; i++) if (!node_eq(&a->kid[i], &b->kid[i])) return 0;
  return 1;
}

/* ------------------------------------------------------------ signature */
static uint32_t crc_table[256];
static int      crc_oom;
static void crc_init(void) {
  if (crc_table[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[n] = c;
  }
}
/* zlib's crc32(crc, buf, len): chaining one call into the next */
static uint32_t crc32z(uint32_t crc, const void *p, size_t n) {
  const uint8_t *b = p;
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
static uint32_t crc_le(uint32_t crc, uint64_t v, int bytes) {
  uint8_t b[8];
  for (int i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i));
  return crc32z(crc, b, (size_t)bytes);
}
static uint32_t fnv1(const char *k, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) { h *= 0x01000193u; h ^= (uint8_t)k[i]; }
  return h;
}
typedef struct { uint32_t h; int i; } HashIdx;
static int cmp_hash(const void *a, const void *b) {
  const HashIdx *x = a, *y = b;
  if (x->h != y->h) return x->h < y->h ? -1 : 1;
  return x->i - y->i;
}
static uint32_t crc_node(uint32_t c, const Node *x) {
  switch (x->t) {
  case T_NULL:  return crc_le(c, 0, 4);
  case T_BOOL:  return crc_le(c, x->raw, 1);
  case T_I32:
  case T_FLOAT: return crc_le(c, x->raw, 4);
  case T_STR:   return crc32z(c, x->s, x->sn);
  case T_ARR:
    c = crc_le(c, (uint32_t)x->n, 4);
    for (int i = 0; i < x->n; i++) c = crc_node(c, &x->kid[i]);
    return c;
  case T_OBJ: {
    c = crc_le(c, (uint32_t)x->n, 4);
    if (!x->n) return c;
    HashIdx *o = malloc((size_t)x->n * sizeof *o);
    if (!o) { crc_oom = 1; return c; }
    for (int i = 0; i < x->n; i++) { o[i].h = fnv1(x->kid[i].key, x->kid[i].kn); o[i].i = i; }
    qsort(o, (size_t)x->n, sizeof *o, cmp_hash);
    for (int i = 0; i < x->n; i++) { c = crc_le(c, o[i].h, 4); c = crc_node(c, &x->kid[o[i].i]); }
    free(o);
    return c;
  }
  default:      return crc_le(c, x->raw, 8);     /* int64, double, DateTime */
  }
}

/* ------------------------------------------------------------ the save */
typedef struct {
  Node root;
  Node *lps, *sps;                     /* localPlayerState, synchronizedPlayerState */
  Node *zone, *ftue, *plays, *height;  /* the signature and what it hides */
  int valid;
} Save;

static uint32_t sig_crc(Save *s) {
  const uint64_t z = s->zone->raw, f = s->ftue->raw;
  crc_init();
  s->zone->raw = s->plays->raw;        /* int32 into int32 */
  s->ftue->raw = s->height->raw;       /* int64 into int64 */
  const uint32_t c = crc_node(SIG_SEED, &s->root);
  s->zone->raw = z; s->ftue->raw = f;
  return c;
}
static int sig_check(Save *s) {
  const uint32_t c = sig_crc(s);
  return (uint32_t)s->zone->raw == c && s->ftue->raw == (uint64_t)c * SIG_MUL;
}
static void sig_sign(Save *s) {
  const uint32_t c = sig_crc(s);
  s->zone->raw = c;
  s->ftue->raw = (uint64_t)c * SIG_MUL;
  s->valid = 1;
}

static void save_free(Save *s) { node_free(&s->root); memset(s, 0, sizeof *s); }

/* Parse; returns 0 and fills *s, or -1 with a reason in err. */
static int save_parse(const uint8_t *b, size_t len, Save *s, char *err, size_t errsz) {
  memset(s, 0, sizeof *s);
  Rd r = { b, len, 0, err, errsz };
  if (rd_node(&r, &s->root, 0)) goto bad;
  if (r.o != len) { snprintf(err, errsz, "%lu unexpected bytes after the end", (unsigned long)(len - r.o)); goto bad; }
  s->lps = child(&s->root, "localPlayerState");
  s->sps = child(&s->root, "synchronizedPlayerState");
  if (s->root.t != T_OBJ || !s->lps || s->lps->t != T_OBJ || !s->sps || s->sps->t != T_OBJ) {
    snprintf(err, errsz, "not a Sonic Jump Fever profile (no player state)");
    goto bad;
  }
  s->zone   = child(s->lps, "zoneState");
  s->plays  = child(s->lps, "totalNumberOfPlays");
  s->height = child(s->lps, "totalHeight");
  s->ftue   = child(&s->root, "ftueStats");
  if (!s->zone || s->zone->t != T_I32 || !s->plays || s->plays->t != T_I32 ||
      !s->ftue || s->ftue->t != T_I64 || !s->height || s->height->t != T_I64) {
    snprintf(err, errsz, "the values the signature is built from are missing or of another type");
    goto bad;
  }
  crc_oom = 0;
  s->valid = sig_check(s);
  if (crc_oom) { snprintf(err, errsz, "out of memory"); goto bad; }
  return 0;
bad:
  node_free(&s->root);
  return -1;
}

/* Serialize with a fresh signature; NULL if out of memory. */
static uint8_t *save_serialize(Save *s, size_t *out_len) {
  crc_oom = 0;
  sig_sign(s);
  Wr w = { 0 };
  w_node(&w, &s->root);
  if (w.oom || crc_oom) { free(w.b); return NULL; }
  *out_len = w.n;
  return w.b;
}

/* ------------------------------------------------------------ characters */
/* The seven playable characters, by the key the save uses. slots = how many
 * power-ups each has (settings.xml); unlock = numTokensRequiredToUnlock.
 * The save also holds vector, rouge and cream, and settings.xml describes
 * them, but they were never finished and never appear in the game, so the
 * editor leaves them out (prop. can still reach their values). */
static const struct { const char *name, *key; int slots, unlock; } k_chars[] = {
  { "Sonic",    "sonic",    4,  20 },
  { "Tails",    "tails",    4,   5 },
  { "Amy",      "amy",      4, 100 },
  { "Knuckles", "knuckles", 4, 250 },
  { "Blaze",    "blaze",    4, 200 },
  { "Shadow",   "shadow",   4, 300 },
  { "Silver",   "silver",   4, 250 },
};
#define N_CHARS ((int)(sizeof k_chars / sizeof k_chars[0]))

/* A character is one int64 (CharacterState), decoded from PlayerProfile:
 *   bit 0          unlocked
 *   bits 1..8      tokens (older field)    } getCharacterTokens = the larger;
 *   bits 32..47    tokens                  } setCharacterTokens writes 32..47
 *   bits 9, 14, 19, 24: four 5-bit power-up levels, one per power-up the
 *                  character has, in settings order (getPowerupLevel caps 6) */
#define MAX_TOKENS   65535
#define MAX_UPGRADE  6
enum { CF_OWNED, CF_TOKENS, CF_UPGRADES };
static const char *const k_cfield[] = { "owned", "tokens", "upgrades" };

static unsigned char_tokens(uint64_t v) {
  const unsigned lo = (unsigned)(v >> 1) & 0xFF, hi = (unsigned)(v >> 32) & 0xFFFF;
  return lo > hi ? lo : hi;
}
static unsigned char_level(uint64_t v, int slot) {
  const unsigned l = (unsigned)(v >> (9 + 5 * slot)) & 0x1F;
  return l > MAX_UPGRADE ? MAX_UPGRADE : l;
}

/* Returns 1 changed, 0 unchanged, -1 skipped. */
static int apply_char(Save *s, int ci, int field, long long num, int quiet) {
  Node *x = child(s->sps, k_chars[ci].key);
  if (!x || x->t != T_I64) {
    if (!quiet) SE_LOG("[saveedit] char.%s: not in profile0.dat -- skipped\n", k_chars[ci].name);
    return -1;
  }
  const uint64_t v = x->raw;
  uint64_t nv = v;
  if (field == CF_OWNED) {
    nv = num ? (v | 1) : (v & ~(uint64_t)1);
  } else if (field == CF_TOKENS) {
    const uint64_t t = (uint64_t)num;
    nv = (v & ~(0xFFFFull << 32)) | (t << 32);
    if (((v >> 1) & 0xFF) > t) nv = (nv & ~(0xFFull << 1)) | (t << 1);   /* the larger counts */
  } else {
    for (int k = 0; k < k_chars[ci].slots; k++) {
      const int sh = 9 + 5 * k;
      nv = (nv & ~(0x1Full << sh)) | ((uint64_t)num << sh);
    }
  }
  if (nv == v) return 0;
  x->raw = nv;
  return 1;
}

/* char.all.<field>: all seven. The caller applies char.all lines FIRST, so a
 * line for one character overrides them wherever it sits in the file. */
static int apply_all(Save *s, const char *key, const char *val, int field, long long num) {
  int n = 0, changed = 0;
  for (int i = 0; i < N_CHARS; i++) {
    const int r = apply_char(s, i, field, num, 1);
    if (r < 0) continue;
    n++; changed += r > 0;
  }
  SE_LOG("[saveedit] %s = %s: %d characters (%d changed)\n", key, val, n, changed);
  return changed ? 1 : 0;
}

/* ------------------------------------------------------------ settings */
/* Friendly names -> a whole-number field of synchronizedPlayerState. The
 * booster names are the shop's (store.json); Quick Fever is numQuickFrenzy. */
static const struct { const char *name, *field; } k_count[] = {
  { "rings",          "numRings"         },
  { "red_star_rings", "numRedRings"      },
  { "energy",         "energyCount"      },
  { "gold_totem",     "numGoldTotem"     },
  { "hoop_boost",     "numHoopBoost"     },
  { "time_extend",    "numTimeExtend"    },
  { "quick_fever",    "numQuickFrenzy"   },
  { "animal_doubler", "numAnimalDoubler" },
  { "ring_streak",    "numRingStreak"    },
  { "power_doubler",  "numPowerDoubler"  },
};
#define N_COUNT ((int)(sizeof k_count / sizeof k_count[0]))

static void node_fmt(const Node *x, char *out, size_t n) {
  switch (x->t) {
  case T_BOOL:  snprintf(out, n, "%s", x->raw ? "true" : "false"); break;
  case T_I32:   snprintf(out, n, "%ld", (long)as_i32(x)); break;
  case T_I64:
  case T_TIME:  snprintf(out, n, "%lld", (long long)(int64_t)x->raw); break;
  case T_FLOAT: { uint32_t u = (uint32_t)x->raw; float f; memcpy(&f, &u, 4); snprintf(out, n, "%.9g", (double)f); break; }
  case T_DOUBLE:{ double d; memcpy(&d, &x->raw, 8); snprintf(out, n, "%.17g", d); break; }
  case T_STR:   snprintf(out, n, "%.*s", (int)x->sn, x->s); break;
  case T_NULL:  snprintf(out, n, "(empty)"); break;
  default:      snprintf(out, n, "(%d values)", x->n); break;
  }
}

/* Set a value from text, keeping its type. 1 changed, 0 same, -1 bad value
 * (*why says what is wrong). */
static int node_set_text(Node *x, const char *v, const char **why) {
  uint64_t nr = x->raw;
  long long q;
  switch (x->t) {
  case T_BOOL: {
    const int b = parse_bool(v);
    if (b < 0) { *why = "use true or false"; return -1; }
    nr = (uint64_t)b;
    break;
  }
  case T_I32:
    if (!parse_ll(v, &q) || q < INT32_MIN || q > INT32_MAX) { *why = "not a whole number from -2147483648 to 2147483647"; return -1; }
    nr = (uint32_t)(int32_t)q;
    break;
  case T_I64:
  case T_TIME:
    if (!parse_ll(v, &q)) { *why = "not a whole number"; return -1; }
    nr = (uint64_t)q;
    break;
  case T_FLOAT:
  case T_DOUBLE: {
    char *e;
    errno = 0;
    const double d = strtod(v, &e);
    if (!*v || *e || errno == ERANGE) { *why = "not a number"; return -1; }
    if (x->t == T_FLOAT) { const float f = (float)d; uint32_t u; memcpy(&u, &f, 4); nr = u; }
    else memcpy(&nr, &d, 8);
    break;
  }
  case T_STR: {
    const size_t l = strlen(v);
    if (l == x->sn && !memcmp(v, x->s, l)) return 0;
    char *ns = malloc(l + 1);
    if (!ns) { *why = "out of memory"; return -1; }
    memcpy(ns, v, l + 1);
    free(x->s); x->s = ns; x->sn = l;
    return 1;
  }
  default:
    *why = x->t == T_NULL ? "it is empty in your save, so it has no type to keep"
                          : "not a single value (it holds several -- see save_contents.txt)";
    return -1;
  }
  if (nr == x->raw) return 0;
  x->raw = nr;
  return 1;
}

/* "a.b.3.c": object members by name (any capitalisation), array elements by
 * index. NULL if any step is missing. */
static Node *find_path(Node *root, const char *path) {
  Node *x = root;
  const char *p = path;
  char seg[256];
  while (*p) {
    const char *dot = strchr(p, '.');
    const size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (!l || l >= sizeof seg || (dot && !dot[1])) return NULL;
    memcpy(seg, p, l); seg[l] = 0;
    Node *c = NULL;
    if (x->t == T_OBJ) {
      if (!(c = child(x, seg)))
        for (int i = 0; i < x->n && !c; i++) if (ieq(x->kid[i].key, seg)) c = &x->kid[i];
    } else if (x->t == T_ARR) {
      long long i;
      if (parse_count(seg, (long long)x->n - 1, &i)) c = &x->kid[i];
    }
    if (!(x = c)) return NULL;
    p = dot ? dot + 1 : p + l;
  }
  return x == root ? NULL : x;
}

static int set_count(const char *key, Node *x, long long n) {
  if (as_i32(x) == n) return 0;
  SE_LOG("[saveedit] %s: %ld -> %lld\n", key, (long)as_i32(x), n);
  x->raw = (uint32_t)(int32_t)n;
  return 1;
}
static int set_flag(const char *key, Node *flags, uint32_t bit, int on) {
  const uint32_t v = (uint32_t)flags->raw, nv = on ? (v | bit) : (v & ~bit);
  if (nv == v) return 0;
  SE_LOG("[saveedit] %s = %s\n", key, on ? "true" : "false");
  flags->raw = nv;
  return 1;
}
static Node *sps_field(Save *s, const char *key, const char *field, uint8_t type) {
  Node *x = child(s->sps, field);
  if (!x || x->t != type) {
    SE_LOG("[saveedit] %s: '%s' is not in profile0.dat -- skipped\n", key, field);
    return NULL;
  }
  return x;
}

/* Apply one "key = value"; returns 1 if the save changed, 0 if not, -1 skipped. */
static int apply_setting(Save *s, const char *key, const char *val) {
  for (int i = 0; i < N_COUNT; i++) {
    if (!ieq(key, k_count[i].name)) continue;
    long long n;
    if (!parse_count(val, INT32_MAX, &n)) {
      SE_LOG("[saveedit] %s = %s: not a whole number from 0 to 2147483647 -- skipped\n", key, val);
      return -1;
    }
    Node *x = sps_field(s, key, k_count[i].field, T_I32);
    return x ? set_count(k_count[i].name, x, n) : -1;
  }
  /* The shop's two permanent upgrades. double_rings is the doubleRings bool
   * (the double_rings product); energy_refill_reducer is awardFlags bit 14,
   * which hasRefillReducer() tests. */
  if (ieq(key, "double_rings") || ieq(key, "energy_refill_reducer")) {
    const int on = parse_bool(val);
    if (on < 0) { SE_LOG("[saveedit] %s = %s: use true or false -- skipped\n", key, val); return -1; }
    if (ieq(key, "double_rings")) {
      Node *x = sps_field(s, key, "doubleRings", T_BOOL);
      if (!x) return -1;
      if ((x->raw != 0) == on) return 0;
      SE_LOG("[saveedit] double_rings = %s\n", on ? "true" : "false");
      x->raw = (uint64_t)on;
      return 1;
    }
    Node *f = sps_field(s, key, "awardFlags", T_I32);
    return f ? set_flag("energy_refill_reducer", f, FLAG_REFILL_REDUCER, on) : -1;
  }
  /* rank: score = (rank << 16) | progress, in the rank format of awardFlags
   * bit 20. The game shows the stored rank + 1. Asking for the rank you are
   * already at keeps your progress towards the next one. */
  if (ieq(key, "rank")) {
    long long r;
    if (!parse_count(val, FEVER_MAX_RANK, &r) || r < 1) {
      SE_LOG("[saveedit] rank = %s: use a rank from 1 to %d -- skipped\n", val, FEVER_MAX_RANK);
      return -1;
    }
    Node *sc = sps_field(s, key, "score", T_I32), *fl = sc ? sps_field(s, key, "awardFlags", T_I32) : NULL;
    if (!fl) return -1;
    const uint32_t f = (uint32_t)fl->raw, cur = (uint32_t)sc->raw;
    if ((f & FLAG_RANK_FORMAT) && (cur >> 16) == (uint32_t)(r - 1)) return 0;
    if (f & FLAG_RANK_FORMAT) SE_LOG("[saveedit] rank: %u -> %lld\n", (cur >> 16) + 1, r);
    else                      SE_LOG("[saveedit] rank: (old score format) -> %lld\n", r);
    sc->raw = (uint32_t)(r - 1) << 16;
    fl->raw = f | FLAG_RANK_FORMAT;
    return 1;
  }
  if (ieqn(key, "char.", 5)) {
    const char *rest = key + 5, *dot = strrchr(rest, '.');
    if (!dot || dot == rest || (size_t)(dot - rest) >= 32) {
      SE_LOG("[saveedit] %s: expected char.<Name>.<owned|tokens|upgrades> -- skipped\n", key);
      return -1;
    }
    char name[32];
    snprintf(name, sizeof name, "%.*s", (int)(dot - rest), rest);
    int field = -1;
    for (int i = 0; i < 3; i++) if (ieq(dot + 1, k_cfield[i])) field = i;
    long long num = 0;
    /* validate the value BEFORE touching the save */
    if (field == CF_OWNED) {
      if ((num = parse_bool(val)) < 0) { SE_LOG("[saveedit] %s = %s: use true or false -- skipped\n", key, val); return -1; }
    } else if (field == CF_TOKENS) {
      if (!parse_count(val, MAX_TOKENS, &num)) { SE_LOG("[saveedit] %s = %s: not a whole number from 0 to %d -- skipped\n", key, val, MAX_TOKENS); return -1; }
    } else if (field == CF_UPGRADES) {
      if (!parse_count(val, MAX_UPGRADE, &num)) { SE_LOG("[saveedit] %s = %s: upgrades go from 0 to %d -- skipped\n", key, val, MAX_UPGRADE); return -1; }
    } else {
      SE_LOG("[saveedit] %s: unknown character field '%s' (owned, tokens, upgrades) -- skipped\n", key, dot + 1);
      return -1;
    }
    if (ieq(name, "all")) return apply_all(s, key, val, field, num);
    int ci = -1;
    for (int i = 0; i < N_CHARS && ci < 0; i++) if (ieq(name, k_chars[i].name)) ci = i;
    if (ci < 0) {
      SE_LOG("[saveedit] %s: '%s' is not a character in Sonic Jump Fever "
             "(see the list at the end of save_edit.txt) -- skipped\n", key, name);
      return -1;
    }
    const int r = apply_char(s, ci, field, num, 0);
    if (r > 0) SE_LOG("[saveedit] char.%s.%s = %s\n", k_chars[ci].name, k_cfield[field], val);
    return r;
  }
  if (ieqn(key, "prop.", 5)) {
    Node *x = find_path(&s->root, key + 5);
    if (!x) {
      SE_LOG("[saveedit] %s: no value '%s' in profile0.dat (see save_contents.txt) -- skipped; nothing is ever added\n", key, key + 5);
      return -1;
    }
    if (x == s->zone || x == s->ftue) {
      SE_LOG("[saveedit] %s: that is the save's signature, which the port sets itself -- skipped\n", key);
      return -1;
    }
    char was[96], now[96];
    const char *why = "";
    node_fmt(x, was, sizeof was);
    const int r = node_set_text(x, val, &why);
    if (r < 0) { SE_LOG("[saveedit] %s = %s: %s -- skipped\n", key, val, why); return -1; }
    if (r > 0) { node_fmt(x, now, sizeof now); SE_LOG("[saveedit] %s: %s -> %s\n", key + 5, was, now); }
    return r;
  }
  SE_LOG("[saveedit] unknown setting '%s' -- skipped\n", key);
  return -1;
}

/* ------------------------------------------------------------ files */
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *b = malloc(SE_MAX_FILE + 1);
  const size_t n = b ? fread(b, 1, SE_MAX_FILE + 1, f) : 0;
  fclose(f);
  if (!b || n > SE_MAX_FILE) { free(b); return NULL; }
  b[n] = 0;
  *len = n;
  return b;
}
static int write_file(const char *path, const void *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(b, 1, n, f) == n;
  return (fclose(f) == 0) && ok;
}
/* temp + rename, as for the save itself */
static int replace_file(const char *path, const void *b, size_t n) {
  char tmp[600];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (!write_file(tmp, b, n)) { remove(tmp); return 0; }
  remove(path);
  if (rename(tmp, path) == 0) return 1;
  const int ok = write_file(path, b, n);
  remove(tmp);
  return ok;
}

/* ------------------------------------------------------------ save_contents.txt */
static int cmp_key(const void *a, const void *b) {
  const Node *x = *(const Node *const *)a, *y = *(const Node *const *)b;
  for (const char *p = x->key, *q = y->key; ; p++, q++) {
    const int c = tolower((unsigned char)*p) - tolower((unsigned char)*q);
    if (c || !*p) return c ? c : strcmp(x->key, y->key);
  }
}
static int printable(const Node *x) {
  for (size_t i = 0; i < x->sn; i++) if ((unsigned char)x->s[i] < 0x20 || (unsigned char)x->s[i] > 0x7E) return 0;
  return 1;
}
/* One line per value, as the prop. path that sets it; members alphabetically. */
static void dump_node(Wr *w, const Save *s, const Node *x, char *path, size_t plen) {
  if (x->t == T_OBJ || x->t == T_ARR) {
    if (!x->n) { w_printf(w, "# %s: empty\n", path); return; }
    const Node **ord = malloc((size_t)x->n * sizeof *ord);
    if (!ord) { w->oom = 1; return; }
    for (int i = 0; i < x->n; i++) ord[i] = &x->kid[i];
    if (x->t == T_OBJ) qsort(ord, (size_t)x->n, sizeof *ord, cmp_key);
    for (int i = 0; i < x->n; i++) {
      const int k = x->t == T_OBJ ? snprintf(path + plen, SE_PATH_CAP - plen, "%s%s", plen ? "." : "", ord[i]->key)
                                  : snprintf(path + plen, SE_PATH_CAP - plen, "%s%d", plen ? "." : "", i);
      if (k > 0 && plen + (size_t)k < SE_PATH_CAP) dump_node(w, s, ord[i], path, plen + (size_t)k);
      path[plen] = 0;
    }
    free(ord);
    return;
  }
  if (x == s->zone || x == s->ftue) { w_printf(w, "# %s: the save's signature (the port sets it)\n", path); return; }
  if (x->t == T_NULL) { w_printf(w, "# %s: empty\n", path); return; }
  if (x->t == T_STR && (!printable(x) || x->sn > 400)) {
    w_printf(w, "# %s: %lu bytes of text that cannot be shown here\n", path, (unsigned long)x->sn);
    return;
  }
  char v[512];
  node_fmt(x, v, sizeof v);
  w_printf(w, "%s = %s\n", path, v);
}

static long sps_int(Save *s, const char *field) {
  Node *x = child(s->sps, field);
  return x && (x->t == T_I32 || x->t == T_BOOL) ? (long)(x->t == T_BOOL ? (int32_t)x->raw : as_i32(x)) : -1;
}

static void write_contents(const char *path, const uint8_t *save, size_t slen) {
  if (!path) return;
  Wr w = { 0 };
  w_printf(&w,
"# save_contents.txt -- what profile0.dat holds, rewritten at every launch\n"
"# (sonicjumpfever_nx). This is a listing only: changing it does nothing.\n"
"# To change a value, copy its line into save_edit.txt with prop. in front:\n"
"#     prop.synchronizedPlayerState.numRings = 5000\n"
"# Times are milliseconds since 1970 (UTC).\n"
"#\n"
"# --- the last launch -----------------------------------------------------\n");
  for (const char *p = g_report; *p; ) {           /* the report, one "#" line each */
    const char *e = strchr(p, '\n');
    const size_t l = e ? (size_t)(e - p) : strlen(p);
    const char *t = p;
    if (!strncmp(t, "[saveedit] ", 11)) t += 11;
    w_printf(&w, "#   %.*s\n", (int)(l - (size_t)(t - p)), t);
    p += l + (e != NULL);
  }
  Save s; char err[160];
  if (save_parse(save, slen, &s, err, sizeof err)) {
    w_printf(&w, "#\n# profile0.dat could not be read: %s\n", err);
  } else {
    w_printf(&w, "#\n# --- in short --------------------------------------------------------------\n");
    if (!s.valid) w_printf(&w, "# THE SIGNATURE DOES NOT MATCH: the game will discard this save.\n");
    const long score = sps_int(&s, "score"), flags = sps_int(&s, "awardFlags");
    if (score >= 0 && flags >= 0 && (flags & FLAG_RANK_FORMAT)) {
      const unsigned rank = ((unsigned long)score >> 16) + 1;
      if (rank >= FEVER_MAX_RANK) w_printf(&w, "# rank %u (the top rank)\n", rank);
      else w_printf(&w, "# rank %u, %lu%% of the way to rank %u\n", rank,
                    (unsigned long)(((unsigned long)score & 0xFFFF) * 100 / 65536), rank + 1);
    }
    w_printf(&w, "# rings %ld, red_star_rings %ld, energy %ld\n",
             sps_int(&s, "numRings"), sps_int(&s, "numRedRings"), sps_int(&s, "energyCount"));
    w_printf(&w, "# boosters:");
    for (int i = 3; i < N_COUNT; i++) w_printf(&w, " %s %ld%s", k_count[i].name, sps_int(&s, k_count[i].field), i + 1 < N_COUNT ? "," : "\n");
    w_printf(&w, "# double_rings %s, energy_refill_reducer %s\n",
             sps_int(&s, "doubleRings") > 0 ? "true" : "false",
             flags >= 0 && (flags & FLAG_REFILL_REDUCER) ? "true" : "false");
    for (int i = 0; i < N_CHARS; i++) {
      Node *x = child(s.sps, k_chars[i].key);
      if (!x || x->t != T_I64) continue;
      const unsigned t = char_tokens(x->raw);
      w_printf(&w, "# %-8s %-7s ", k_chars[i].name, (x->raw & 1) ? "owned," : "locked,");
      if (x->raw & 1) w_printf(&w, "%u tokens", t);
      else w_printf(&w, "%u of %d tokens", t, k_chars[i].unlock);
      w_printf(&w, ", upgrades");
      for (int k = 0; k < k_chars[i].slots; k++) w_printf(&w, " %u", char_level(x->raw, k));
      w_printf(&w, "\n");
    }
    w_printf(&w, "#\n# --- everything ------------------------------------------------------------\n");
    char p[SE_PATH_CAP] = "";
    dump_node(&w, &s, &s.root, p, 0);
    save_free(&s);
  }
  if (w.oom) { free(w.b); return; }
  size_t olen = 0;
  char *old = read_file(path, &olen);
  const int same = old && olen == w.n && !memcmp(old, w.b, w.n);
  free(old);
  if (!same) {
    if (replace_file(path, w.b, w.n)) g_wrote = 1;
    else SE_LOG("[saveedit] could not write %s\n", path);
  }
  free(w.b);
}

/* ------------------------------------------------------------ save_edit.txt */
#define ALL_MARKER    "# --- every character at once ---"
#define ROSTER_MARKER "# --- all characters ---"

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { SE_LOG("[saveedit] could not write %s\n", path); return; }
  fputs(
"# save_edit.txt -- Sonic Jump Fever save editing (sonicjumpfever_nx).\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to profile0.dat at EVERY launch, for as long as the line stays\n"
"# uncommented -- \"rings = 50000\" puts you back at 50000 rings each launch.\n"
"# Comment it out again once the change has taken.\n"
"#\n"
"# profile0.dat is protected by a signature: editing it directly makes the game\n"
"# throw your progress away. Editing here is safe -- the port re-signs the save\n"
"# exactly as the game does, and keeps the untouched original once as\n"
"# profile0.dat.orig.\n"
"#\n"
"# Only values your save already holds are changed; nothing is ever added.\n"
"# save_contents.txt, rewritten at every launch, shows what was applied (or why\n"
"# not) at the top, then everything the save holds with its current value.\n"
"# Quit the game fully before editing: it saves on the way out.\n"
"\n"
"# --- currencies ----------------------------------------------------------\n"
"#rings = 50000\n"
"#red_star_rings = 500\n"
"# energy refills by itself up to 5; more than 5 is kept until you play.\n"
"#energy = 5\n"
"\n"
"# --- rank ----------------------------------------------------------------\n"
"# The rank the game shows, from 1 to 52. Progress towards the next rank\n"
"# starts from zero, and the rewards for ranks you skip are not handed out.\n"
"#rank = 10\n"
"\n"
"# --- boosters ------------------------------------------------------------\n"
"# How many of each booster you hold.\n"
"#gold_totem = 10\n"
"#hoop_boost = 10\n"
"#time_extend = 10\n"
"#quick_fever = 10\n"
"#animal_doubler = 10\n"
"#ring_streak = 10\n"
"#power_doubler = 10\n"
"\n"
"# --- upgrades ------------------------------------------------------------\n"
"# The two permanent upgrades the game sold: double_rings doubles the rings you\n"
"# collect; energy_refill_reducer makes energy refill faster. The shop is off\n"
"# on Switch, so this is how to get them. false takes either away again.\n"
"#double_rings = true\n"
"#energy_refill_reducer = true\n"
"\n"
"# --- characters ----------------------------------------------------------\n"
"# char.<Name>.owned = true unlocks a character outright.\n"
"# char.<Name>.tokens sets their tokens (what unlocks them in the game).\n"
"# char.<Name>.upgrades sets every one of their power-ups to that level, 0-6.\n"
"# Every character is listed at the end of this file; any capitalisation works.\n"
"# char.all.owned / .tokens / .upgrades do all of them at once.\n"
"#char.Knuckles.owned = true\n"
"#char.Sonic.upgrades = 6\n"
"\n"
"# --- anything else -------------------------------------------------------\n"
"# prop.<path> = value sets any single value already in profile0.dat, keeping\n"
"# its type. Every path and its current value is in save_contents.txt; numbers\n"
"# in a path pick an item from a list.\n"
"#prop.synchronizedPlayerState.numCharTokens = 16\n"
"\n"
"# --- repair --------------------------------------------------------------\n"
"# If profile0.dat was changed by something else, the game would throw it away.\n"
"# Uncomment this to have the port re-sign it instead -- only when its layout is\n"
"# intact; the copy before repair is kept as profile0.dat.orig.\n"
"#fix_hand_edited_save = true\n"
"\n"
ALL_MARKER "--------------------------------------\n"
"# char.all.<field> sets it for all seven characters. A line for one character\n"
"# overrides it, wherever it is in the file.\n"
"#char.all.owned = true\n"
"#char.all.upgrades = 6\n"
"\n"
ROSTER_MARKER "-------------------------------------------------\n"
"# The seven characters in Sonic Jump Fever 1.6.1, with the tokens each one\n"
"# takes to unlock in the game.\n", f);
  for (int i = 0; i < N_CHARS; i++)
    fprintf(f, "#char.%s.owned = true\n#char.%s.tokens = %d\n#char.%s.upgrades = 6\n",
            k_chars[i].name, k_chars[i].name, k_chars[i].unlock, k_chars[i].name);
  fclose(f);
  g_wrote = 1;
  SE_LOG("[saveedit] wrote save_edit.txt (every setting commented out)\n");
}

/* ------------------------------------------------------------ entry */
static int run(const char *save_path, const char *edit_path,
               uint8_t **final, size_t *final_len) {
  size_t elen = 0;
  char *edit = read_file(edit_path, &elen);
  if (!edit) write_template(edit_path);

  /* Collect uncommented settings first. */
  static struct { char *k, *v; } set[256];
  int ns = 0, fix = 0, dropped = 0;
  if (edit) {
    char *text = edit;
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;     /* Notepad's UTF-8 mark */
    for (char *ln = strtok(text, "\n"); ln; ln = strtok(NULL, "\n")) {
      char *t = trim(ln);
      if (!*t || *t == '#') continue;
      char *eq = strchr(t, '=');
      if (!eq) { SE_LOG("[saveedit] ignoring '%.80s' (no '=')\n", t); continue; }
      *eq = 0;
      char *k = trim(t), *v = trim(eq + 1);
      if (ieq(k, "fix_hand_edited_save")) { fix = parse_bool(v) == 1; continue; }
      if (!is_ascii_line(v)) { SE_LOG("[saveedit] %s: value must be one line of plain ASCII -- skipped\n", k); continue; }
      if (ns < (int)(sizeof set / sizeof set[0])) { set[ns].k = k; set[ns].v = v; ns++; }
      else dropped++;
    }
  }
  if (dropped) SE_LOG("[saveedit] more than %d settings -- the last %d ignored\n", (int)(sizeof set / sizeof set[0]), dropped);

  size_t slen = 0;
  uint8_t *raw = (uint8_t *)read_file(save_path, &slen);
  if (!raw) {
    SE_LOG("[saveedit] no profile0.dat yet -- play once, quit, then edit\n");
    free(edit);
    return 0;
  }
  *final = raw; *final_len = slen;                 /* what save_contents.txt lists */
  Save s; char err[160];
  if (save_parse(raw, slen, &s, err, sizeof err)) {
    SE_LOG("[saveedit] profile0.dat %s: %s\n", ns || fix ? "not edited" : "could not be read", err);
    free(edit);
    return ns || fix ? -1 : 0;
  }
  const int was_valid = s.valid;
  if (!s.valid && !fix) {
    SE_LOG("[saveedit] profile0.dat's signature does not match (changed by something else?) -- not touched. "
           "The game will discard it; set fix_hand_edited_save = true in save_edit.txt to re-sign it instead.\n");
    save_free(&s); free(edit);
    return ns ? -1 : 0;
  }
  if (!ns && was_valid) { save_free(&s); free(edit); return 0; }

  int changed = !was_valid, applied = 0;           /* a repair is itself a change */
  /* char.all lines first, then everything else: a line for one character
   * overrides char.all wherever it sits in the file */
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < ns; i++) {
      if (ieqn(set[i].k, "char.all.", 9) != (pass == 0)) continue;
      const int r = apply_setting(&s, set[i].k, set[i].v);
      if (r >= 0) applied++;
      if (r > 0) changed = 1;
    }
  if (!changed) {
    SE_LOG(applied ? "[saveedit] settings already match profile0.dat -- nothing written\n"
                   : "[saveedit] no setting could be applied (see above) -- nothing written\n");
    save_free(&s); free(edit);
    return 0;
  }

  size_t olen = 0;
  uint8_t *out = save_serialize(&s, &olen);
  int ok = out != NULL;
  if (ok) {                                        /* 3. read it back before trusting it */
    Save chk; char e2[160];
    ok = !save_parse(out, olen, &chk, e2, sizeof e2) && chk.valid && node_eq(&chk.root, &s.root);
    if (!ok) SE_LOG("[saveedit] verification of the new save FAILED -- nothing written\n");
    if (chk.root.t) save_free(&chk);
  } else {
    SE_LOG("[saveedit] out of memory -- nothing written\n");
  }
  if (ok) {
    char orig[600];
    snprintf(orig, sizeof orig, "%s.orig", save_path);
    FILE *o = fopen(orig, "rb");                   /* 4. keep the untouched original once */
    if (o) fclose(o);
    else if (!write_file(orig, raw, slen)) SE_LOG("[saveedit] could not keep profile0.dat.orig\n");
    ok = replace_file(save_path, out, olen);       /* 5. temp + rename */
    g_wrote = 1;
    SE_LOG(ok ? "[saveedit] profile0.dat %s and re-signed\n" : "[saveedit] could not write profile0.dat\n",
           was_valid ? "edited" : "repaired");
  }
  if (ok) { *final = out; *final_len = olen; free(raw); }
  else free(out);
  save_free(&s); free(edit);
  return ok ? 1 : -1;
}

int sj_saveedit_run_paths(const char *save_path, const char *edit_path, const char *contents_path) {
  g_rlen = 0; g_report[0] = 0; g_wrote = 0;
  uint8_t *final = NULL; size_t final_len = 0;
  const int r = run(save_path, edit_path, &final, &final_len);
  if (final) write_contents(contents_path, final, final_len);
  free(final);
#ifdef __SWITCH__
  if (g_wrote) fsdevCommitDevice("sdmc");
#endif
  return r;
}

#ifdef __SWITCH__
void sj_saveedit_run(void) {
#if SJ_SAVE_EDIT
  char sp[512], ep[512], cp[512];
  sj_path(sp, sizeof sp, SAVE_NAME);
  sj_path(ep, sizeof ep, SAVE_EDIT_NAME);
  sj_path(cp, sizeof cp, SAVE_CONTENTS_NAME);
  sj_saveedit_run_paths(sp, ep, cp);
#endif
}
#endif

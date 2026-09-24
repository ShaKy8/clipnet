#include "transform.h"

#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

#include "util.h"

static const struct transform_def defs[] = {
  { "upper", "UPPER CASE" },
  { "lower", "lower case" },
  { "title", "Title Case" },
  { "sentence", "Sentence case" },
  { "invert", "iNVERT cASE" },
  { "camel", "camelCase" },
  { "snake", "snake_case" },
  { "slug", "slugify-text" },
  { "trim", "Trim whitespace" },
  { "one_line", "Remove line breaks" },
  { "add_newline", "Add a line break" },
  { "add_two_newlines", "Add two line breaks" },
  { "append_date", "Append date and time" },
  { "date", "Date and time only" },
};

const struct transform_def *transform_list(size_t *n)
{
  *n = ARRAY_LEN(defs);
  return defs;
}

void transform_init_locale(void)
{
  if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8"))
    log_warn("no UTF-8 locale; case transforms will only change ASCII letters");
}

/* ---- UTF-8 <-> code points ---------------------------------------------- */

struct cps {
  uint32_t *v;
  size_t n, cap;
};

static void cps_push(struct cps *c, uint32_t cp)
{
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 64;
    c->v = xrealloc(c->v, c->cap * sizeof *c->v);
  }
  c->v[c->n++] = cp;
}

/* Invalid sequences become U+FFFD; the input is normally valid already. */
static struct cps decode(const char *s, size_t n)
{
  struct cps c = { 0 };
  size_t i = 0;
  while (i < n) {
    uint8_t b = (uint8_t)s[i];
    uint32_t cp;
    size_t len;
    if (b < 0x80) { cp = b; len = 1; }
    else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
    else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
    else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
    else { cps_push(&c, 0xFFFD); i++; continue; }
    if (i + len > n) { cps_push(&c, 0xFFFD); break; }
    bool ok = true;
    for (size_t k = 1; k < len; k++) {
      if (((uint8_t)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
    }
    if (!ok) { cps_push(&c, 0xFFFD); i++; continue; }
    cps_push(&c, cp);
    i += len;
  }
  return c;
}

static void put(struct buf *b, uint32_t cp)
{
  char o[4];
  if (cp < 0x80) { o[0] = (char)cp; buf_append(b, o, 1); }
  else if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); buf_append(b, o, 2); }
  else if (cp < 0x10000) {
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    buf_append(b, o, 3);
  } else {
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    buf_append(b, o, 4);
  }
}

static char *encode(const struct cps *c)
{
  struct buf b = { 0 };
  for (size_t i = 0; i < c->n; i++) put(&b, c->v[i]);
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

static uint32_t up(uint32_t c) { return (uint32_t)towupper((wint_t)c); }
static uint32_t low(uint32_t c) { return (uint32_t)towlower((wint_t)c); }
static bool alpha(uint32_t c) { return iswalpha((wint_t)c); }
static bool alnum(uint32_t c) { return iswalnum((wint_t)c); }
static bool space(uint32_t c) { return iswspace((wint_t)c); }

/* An apostrophe between letters belongs to the word ("don't", "l'été"). */
static bool joins_word(const struct cps *c, size_t i)
{
  uint32_t x = c->v[i];
  return (x == '\'' || x == 0x2019) && i > 0 && i + 1 < c->n && alpha(c->v[i - 1]) && alpha(c->v[i + 1]);
}

/* ---- transforms --------------------------------------------------------- */

static void t_title(struct cps *c)
{
  bool in_word = false;
  for (size_t i = 0; i < c->n; i++) {
    if (alnum(c->v[i]) || (in_word && joins_word(c, i))) {
      c->v[i] = in_word ? low(c->v[i]) : up(c->v[i]);
      in_word = true;
    } else {
      in_word = false;
    }
  }
}

/* Lower-case everything, then capitalise the first letter of the text and
 * of every sentence (after . ! ? and whitespace, or after a blank line). */
static void t_sentence(struct cps *c)
{
  bool start = true, after_end = false;
  for (size_t i = 0; i < c->n; i++) {
    uint32_t x = c->v[i];
    if (alpha(x)) {
      c->v[i] = start ? up(x) : low(x);
      start = after_end = false;
    } else if (x == '.' || x == '!' || x == '?') {
      after_end = true;
    } else if (space(x)) {
      if (after_end || (x == '\n' && i + 1 < c->n && c->v[i + 1] == '\n')) start = true;
    } else if (!alnum(x)) {
      /* Quotes and brackets keep a pending capital pending. */
    } else {
      start = after_end = false; /* a digit starts the sentence */
    }
  }
}

static void t_invert(struct cps *c)
{
  for (size_t i = 0; i < c->n; i++) {
    uint32_t x = c->v[i], u = up(x), l = low(x);
    c->v[i] = x == u ? l : u;
  }
}

/* Words of letters and digits, split at everything else and at
 * lower→upper case changes ("fooBar baz" → foo, bar, baz). */
static char *words_joined(const struct cps *c, bool camel, uint32_t sep)
{
  struct cps out = { 0 };
  size_t word = 0;
  bool in_word = false;
  for (size_t i = 0; i < c->n; i++) {
    uint32_t x = c->v[i];
    bool boundary = in_word && i > 0 && alpha(c->v[i - 1]) && c->v[i - 1] == low(c->v[i - 1]) && alpha(x) &&
                    x != low(x);
    if (!alnum(x) && !joins_word(c, i)) { in_word = false; continue; }
    if (joins_word(c, i)) continue; /* "don't" → "dont" */
    if (!in_word || boundary) {
      if (word && !camel) cps_push(&out, sep);
      cps_push(&out, camel ? (word ? up(x) : low(x)) : low(x));
      word++;
      in_word = true;
      continue;
    }
    cps_push(&out, low(x));
  }
  char *s = encode(&out);
  free(out.v);
  return s;
}

static char *t_trim(const char *in, size_t len)
{
  size_t a = 0, b = len;
  while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\n' || in[a] == '\r' || in[a] == '\f' || in[a] == '\v')) a++;
  while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\n' || in[b - 1] == '\r' || in[b - 1] == '\f' ||
                   in[b - 1] == '\v'))
    b--;
  return xstrndup(in + a, b - a);
}

/* Line breaks become single spaces; spaces around a break do not pile up. */
static char *t_one_line(const char *in, size_t len)
{
  struct buf b = { 0 };
  size_t i = 0;
  while (i < len) {
    if (in[i] == '\r' || in[i] == '\n') {
      while (b.len && (b.data[b.len - 1] == ' ' || b.data[b.len - 1] == '\t')) b.data[--b.len] = 0;
      while (i < len && (in[i] == '\r' || in[i] == '\n' || in[i] == ' ' || in[i] == '\t')) i++;
      if (b.len && i < len) buf_append(&b, " ", 1);
      continue;
    }
    buf_append(&b, in + i, 1);
    i++;
  }
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

static char *now_str(const char *fmt)
{
  char out[256];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  size_t n = strftime(out, sizeof out, fmt && *fmt ? fmt : "%Y-%m-%d %H:%M", &tm);
  return xstrndup(out, n);
}

char *transform_apply(const char *id, const char *in, size_t len, const char *date_fmt, const char **err)
{
  if (!strcmp(id, "trim")) return t_trim(in, len);
  if (!strcmp(id, "one_line")) return t_one_line(in, len);
  if (!strcmp(id, "add_newline") || !strcmp(id, "add_two_newlines")) {
    char *s = xmalloc(len + 3);
    memcpy(s, in, len);
    size_t n = len;
    s[n++] = '\n';
    if (id[4] == 't') s[n++] = '\n';
    s[n] = 0;
    return s;
  }
  if (!strcmp(id, "date")) return now_str(date_fmt);
  if (!strcmp(id, "append_date")) {
    char *d = now_str(date_fmt);
    char *s = xasprintf("%.*s %s", (int)len, in, d);
    free(d);
    return s;
  }

  struct cps c = decode(in, len);
  char *out = NULL;
  if (!strcmp(id, "upper")) { for (size_t i = 0; i < c.n; i++) c.v[i] = up(c.v[i]); }
  else if (!strcmp(id, "lower")) { for (size_t i = 0; i < c.n; i++) c.v[i] = low(c.v[i]); }
  else if (!strcmp(id, "title")) t_title(&c);
  else if (!strcmp(id, "sentence")) t_sentence(&c);
  else if (!strcmp(id, "invert")) t_invert(&c);
  else if (!strcmp(id, "camel")) out = words_joined(&c, true, 0);
  else if (!strcmp(id, "snake")) out = words_joined(&c, false, '_');
  else if (!strcmp(id, "slug")) out = words_joined(&c, false, '-');
  else {
    free(c.v);
    *err = "unknown transform";
    return NULL;
  }
  if (!out) out = encode(&c);
  free(c.v);
  return out;
}

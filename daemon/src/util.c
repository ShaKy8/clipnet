#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum log_level log_threshold = LOG_INFO;

void log_msg(enum log_level lvl, const char *fmt, ...)
{
  static const char *names[] = { "error", "warn", "info", "debug" };
  if (lvl > log_threshold) return;
  va_list ap;
  va_start(ap, fmt);
  /* journald adds its own timestamps; stderr is the log. */
  fprintf(stderr, "clipnetd %s: ", names[lvl]);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static void oom(void)
{
  fputs("clipnetd: out of memory\n", stderr);
  abort();
}

void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);
  if (!p) oom();
  return p;
}

void *xcalloc(size_t n, size_t sz)
{
  void *p = calloc(n ? n : 1, sz ? sz : 1);
  if (!p) oom();
  return p;
}

void *xrealloc(void *p, size_t n)
{
  p = realloc(p, n ? n : 1);
  if (!p) oom();
  return p;
}

char *xstrdup(const char *s)
{
  return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  return p;
}

char *xasprintf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *out = NULL;
  if (vasprintf(&out, fmt, ap) < 0) oom();
  va_end(ap);
  return out;
}

void buf_reserve(struct buf *b, size_t extra)
{
  if (b->len + extra + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->len + extra + 1) cap *= 2;
  b->data = xrealloc(b->data, cap);
  b->cap = cap;
}

void buf_append(struct buf *b, const void *p, size_t n)
{
  buf_reserve(b, n);
  if (n) memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = 0;
}

void buf_append_str(struct buf *b, const char *s)
{
  buf_append(b, s, strlen(s));
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) { va_end(ap2); return; }
  buf_reserve(b, (size_t)n);
  vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->len += (size_t)n;
}

void buf_free(struct buf *b)
{
  free(b->data);
  *b = (struct buf){ 0 };
}

char *buf_steal(struct buf *b, size_t *len)
{
  buf_reserve(b, 0);
  char *p = (char *)b->data;
  if (len) *len = b->len;
  *b = (struct buf){ 0 };
  return p;
}

int64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int mkdir_p(const char *path, unsigned mode)
{
  char *p = xstrdup(path);
  for (char *s = p + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    if (mkdir(p, mode) < 0 && errno != EEXIST) { free(p); return -1; }
    *s = '/';
  }
  int r = mkdir(p, mode);
  free(p);
  return r < 0 && errno != EEXIST ? -1 : 0;
}

char *xdg_path(const char *env, const char *fallback_rel, const char *suffix)
{
  const char *base = getenv(env);
  if (base && base[0] == '/') return xasprintf("%s/%s", base, suffix);
  const char *home = getenv("HOME");
  if (!home) home = "/tmp";
  return xasprintf("%s/%s/%s", home, fallback_rel, suffix);
}

bool utf8_valid(const uint8_t *s, size_t n)
{
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    size_t len;
    uint32_t cp;
    if (c < 0x80) { i++; continue; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    /* Reject overlongs, surrogates and out-of-range code points. */
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

char *latin1_to_utf8(const uint8_t *s, size_t n, size_t *out_len)
{
  char *out = xmalloc(n * 2 + 1);
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < 0x80) out[o++] = (char)s[i];
    else {
      out[o++] = (char)(0xC0 | (s[i] >> 6));
      out[o++] = (char)(0x80 | (s[i] & 0x3F));
    }
  }
  out[o] = 0;
  if (out_len) *out_len = o;
  return out;
}

size_t utf8_prefix(const char *s, size_t n, size_t max_chars)
{
  size_t i = 0, chars = 0;
  while (i < n && chars < max_chars) {
    uint8_t c = (uint8_t)s[i];
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (i + len > n) break;
    i += len;
    chars++;
  }
  return i;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = hex[in[i] >> 4];
    out[2 * i + 1] = hex[in[i] & 15];
  }
  out[2 * n] = 0;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const uint8_t *in, size_t n)
{
  char *out = xmalloc(4 * ((n + 2) / 3) + 1);
  size_t o = 0, i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = b64[v >> 18];
    out[o++] = b64[(v >> 12) & 63];
    out[o++] = b64[(v >> 6) & 63];
    out[o++] = b64[v & 63];
  }
  if (i < n) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
    out[o++] = b64[v >> 18];
    out[o++] = b64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
    out[o++] = '=';
  }
  out[o] = 0;
  return out;
}

uint8_t *base64_decode(const char *in, size_t *out_len)
{
  size_t n = strlen(in);
  uint8_t *out = xmalloc(n / 4 * 3 + 3);
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0, pad = 0;
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    if (c == '=') { pad++; continue; }
    if (pad) { free(out); return NULL; } /* data after padding */
    const char *p = strchr(b64, c);
    if (!p || !c) { free(out); return NULL; }
    acc = acc << 6 | (uint32_t)(p - b64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  if (pad > 2 || bits >= 6) { free(out); return NULL; }
  *out_len = o;
  return out;
}

void uuid_v4(char out[37])
{
  uint8_t b[16];
  if (getrandom(b, sizeof b, 0) != sizeof b) {
    /* getrandom on Linux ≥3.17 does not fail for 16 bytes once the pool is
     * initialised; fall back to a time-seeded mix rather than aborting. */
    uint64_t x = (uint64_t)now_ms() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < 16; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; b[i] = (uint8_t)x; }
  }
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int set_nonblock(int fd)
{
  int fl = fcntl(fd, F_GETFL);
  return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

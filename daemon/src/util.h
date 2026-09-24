/*
 * Small shared helpers: logging, growable buffers, time, paths.
 *
 * Logging never takes clip content. Callers pass ids, mime types, sizes and
 * app classes only; there is deliberately no helper that formats a payload.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum log_level { LOG_ERR, LOG_WARN, LOG_INFO, LOG_DEBUG };
extern enum log_level log_threshold;
void log_msg(enum log_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define log_err(...) log_msg(LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A growable byte buffer. data is always NUL-terminated past len so text
 * payloads can be used as C strings without a copy. */
struct buf {
  uint8_t *data;
  size_t len, cap;
};
void buf_reserve(struct buf *b, size_t extra);
void buf_append(struct buf *b, const void *p, size_t n);
void buf_append_str(struct buf *b, const char *s);
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(struct buf *b);
/* Hand the storage to the caller (NUL-terminated) and reset the buffer. */
char *buf_steal(struct buf *b, size_t *len);

int64_t now_ms(void);      /* wall clock, Unix ms UTC: stored timestamps */
int64_t mono_ms(void);     /* monotonic: timers and timeouts */

/* mkdir -p with the given mode for every created component. */
int mkdir_p(const char *path, unsigned mode);
/* $XDG_x_HOME with the spec's fallbacks, joined with suffix. */
char *xdg_path(const char *env, const char *fallback_rel, const char *suffix);

bool utf8_valid(const uint8_t *s, size_t n);
/* Latin-1 → UTF-8 (for X11 STRING payloads). */
char *latin1_to_utf8(const uint8_t *s, size_t n, size_t *out_len);
/* Length in bytes of the longest prefix of s holding at most max_chars code
 * points, never splitting a sequence. s must be valid UTF-8. */
size_t utf8_prefix(const char *s, size_t n, size_t max_chars);

void hex_encode(const uint8_t *in, size_t n, char *out /* 2n+1 */);
void uuid_v4(char out[37]);
int set_nonblock(int fd);

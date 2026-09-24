/*
 * Special Paste: text transforms applied at paste time (the stored clip is
 * never changed). Unicode-aware through the C library's wide-character case
 * tables, so the daemon runs with LC_CTYPE=C.UTF-8. Mapping is one code
 * point to one code point: "ß" upper-cases to "ß", not "SS".
 */
#pragma once

#include <stddef.h>

struct transform_def {
  const char *id;
  const char *label;
};

const struct transform_def *transform_list(size_t *n);

/* Returns a malloc'd, NUL-terminated result, or NULL with *err set for an
 * unknown id. date_fmt is a strftime format for the date transforms. */
char *transform_apply(const char *id, const char *in, size_t len, const char *date_fmt, const char **err);

/* Call once at startup: the case tables need a UTF-8 LC_CTYPE. */
void transform_init_locale(void);

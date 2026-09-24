/* Image dimensions from file headers, without an image library. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct img_meta {
  const char *format; /* "PNG", "JPEG", ... static string */
  uint32_t width, height;
};

bool img_meta_parse(const uint8_t *p, size_t n, struct img_meta *out);

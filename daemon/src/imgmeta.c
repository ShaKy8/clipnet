#include "imgmeta.h"

#include <string.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[1] << 8 | p[0]; }
static uint32_t le24(const uint8_t *p) { return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

static bool jpeg(const uint8_t *p, size_t n, struct img_meta *o)
{
  size_t i = 2;
  while (i + 9 < n) {
    if (p[i] != 0xFF) return false;
    uint8_t m = p[i + 1];
    if (m == 0xFF) { i++; continue; } /* fill byte */
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
    uint32_t len = be16(p + i + 2);
    /* SOF0..SOF15 carry the frame size, except DHT (C4), JPG (C8), DAC (CC). */
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      o->height = be16(p + i + 5);
      o->width = be16(p + i + 7);
      return o->width && o->height;
    }
    if (len < 2) return false;
    i += 2 + len;
  }
  return false;
}

bool img_meta_parse(const uint8_t *p, size_t n, struct img_meta *o)
{
  memset(o, 0, sizeof *o);
  if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4)) {
    o->format = "PNG";
    o->width = be32(p + 16);
    o->height = be32(p + 20);
    return true;
  }
  if (n >= 10 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) {
    o->format = "GIF";
    o->width = le16(p + 6);
    o->height = le16(p + 8);
    return true;
  }
  if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
    o->format = "JPEG";
    return jpeg(p, n, o);
  }
  if (n >= 30 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) {
    o->format = "WEBP";
    if (!memcmp(p + 12, "VP8X", 4)) {
      o->width = le24(p + 24) + 1;
      o->height = le24(p + 27) + 1;
      return true;
    }
    if (!memcmp(p + 12, "VP8L", 4) && p[20] == 0x2F) {
      uint32_t b = le32(p + 21);
      o->width = (b & 0x3FFF) + 1;
      o->height = ((b >> 14) & 0x3FFF) + 1;
      return true;
    }
    if (!memcmp(p + 12, "VP8 ", 4) && p[23] == 0x9D && p[24] == 0x01 && p[25] == 0x2A) {
      o->width = le16(p + 26) & 0x3FFF;
      o->height = le16(p + 28) & 0x3FFF;
      return true;
    }
    return false;
  }
  if (n >= 26 && p[0] == 'B' && p[1] == 'M') {
    o->format = "BMP";
    int32_t w = (int32_t)le32(p + 18), h = (int32_t)le32(p + 22);
    o->width = (uint32_t)(w < 0 ? -w : w);
    o->height = (uint32_t)(h < 0 ? -h : h);
    return true;
  }
  return false;
}

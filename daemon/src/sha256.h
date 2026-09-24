/* SHA-256 (FIPS 180-4). Used for clip identity and blob file names. */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct sha256 {
  uint32_t h[8];
  uint64_t total;
  uint8_t block[64];
  size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);

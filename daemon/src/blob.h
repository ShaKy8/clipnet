/*
 * Content-addressed payload files: <dir>/<first 2 hex>/<sha256 hex>.
 *
 * Images always live here (the UI loads thumbnails straight from the path);
 * other formats only when they are too big to sit inline in SQLite.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Relative name "ab/abcd..." for a hash; out must hold 68 bytes. */
void blob_name(const uint8_t hash[32], char out[68]);
/* Write if absent (atomic: tmp + rename). 0 on success. */
int blob_put(const char *dir, const char *name, const void *data, size_t n);
/* Map read-only. Returns NULL on error; unmap with blob_unmap. */
void *blob_map(const char *dir, const char *name, size_t *len);
void blob_unmap(void *p, size_t len);
char *blob_path(const char *dir, const char *name);
int blob_unlink(const char *dir, const char *name);

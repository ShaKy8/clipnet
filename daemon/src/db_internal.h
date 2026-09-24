/* Shared between the db_*.c files; not part of the store's API. */
#pragma once

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

#include "db.h"
#include "mime.h"

struct db {
  sqlite3 *h;
  char *dir;
  char *blob_dir;
};

struct derived {
  enum clip_kind kind;
  char *plain; /* NULL when the clip has no text at all */
  char *preview;
  int flags;
};

sqlite3_stmt *dbi_prep(struct db *db, const char *sql);
int dbi_exec(struct db *db, const char *sql);
void dbi_derive(const struct clip_in *in, struct derived *d);
void dbi_content_hash(const struct clip_in *in, uint8_t out[32]);
int dbi_insert_formats(struct db *db, int64_t clip_id, const struct fmt_in *fmts, size_t n);
bool dbi_blob_referenced(struct db *db, const char *name);

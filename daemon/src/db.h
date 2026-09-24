/*
 * The clip store: SQLite for rows and small payloads, blob files for the rest.
 * The daemon is the only writer; everything else goes through its socket.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;
typedef struct sqlite3 sqlite3;
struct db;

/* clips.flags bits */
#define CLIP_FLAG_TEXT_IS_HTML 1 /* the "plain text" format held the HTML markup itself */

struct db *db_open(const char *dir);
void db_close(struct db *db);
sqlite3 *db_handle(struct db *db);
const char *db_dir(struct db *db);
const char *db_blob_dir(struct db *db);

/* One captured format. aliases: the names the source offered this format
 * under (text only), or NULL. */
struct fmt_in {
  const char *mime;
  const uint8_t *data;
  size_t len;
  const char *const *aliases;
  size_t n_aliases;
};

struct clip_in {
  struct fmt_in *fmts;
  size_t n_fmts;
  const char *source_app;   /* may be NULL */
  const char *source_title; /* may be NULL */
  int64_t created_at;       /* 0: now */
  int64_t last_used_at;     /* 0: created_at */
  const char *uuid;         /* NULL: new random uuid */
  const char *title;        /* NULL: derived from content */
  int64_t group_id;         /* 0: none */
};

enum add_result { ADD_ERR = -1, ADD_NEW = 0, ADD_DUP = 1 };
/* Store a clip. An identical clip (same formats, same bytes) is moved to the
 * top instead and ADD_DUP returned with its id. */
enum add_result db_add_clip(struct db *db, const struct clip_in *in, int64_t *id);

/* Everything needed to offer a clip on the clipboard. Blob-backed formats are
 * mapped read-only, never copied. */
struct fmt_out {
  char *mime;
  char **aliases;
  size_t n_aliases;
  const uint8_t *data;
  size_t len;
  size_t map_len; /* nonzero: data is an mmap; zero: data is malloc(ed) */
};
struct clip_payload {
  int64_t id;
  char uuid[37];
  struct fmt_out *fmts;
  size_t n_fmts;
};
int db_load_payload(struct db *db, int64_t id, struct clip_payload *out);
void clip_payload_free(struct clip_payload *p);
/* The clip's text (MIME_TEXT format, else its derived plain text). Caller
 * frees. NULL if it has none. */
char *db_clip_text(struct db *db, int64_t id, size_t *len);

struct list_query {
  const char *query;  /* NULL or "": no search */
  int64_t group_id;   /* 0: history (all or ungrouped, per setting) */
  bool all_groups;    /* search across every group regardless of scope */
  int offset, limit;
};
/* {"rows": [...], "total": N} */
cJSON *db_list(struct db *db, const struct list_query *q);
/* Ids at positions (0-based) in the history view, sticky clips first. */
int64_t db_id_at_position(struct db *db, int pos);
/* Full detail of one clip: row fields + formats + full text. NULL: absent. */
cJSON *db_get(struct db *db, int64_t id);
/* Row fields only, as in db_list. */
cJSON *db_row(struct db *db, int64_t id);
bool db_exists(struct db *db, int64_t id);
/* 0 when found. */
int db_clip_uuid(struct db *db, int64_t id, char out[37]);

int db_delete(struct db *db, int64_t id);
/* Move to the top of history; count a paste when pasted is true. */
int db_touch(struct db *db, int64_t id, bool pasted);

/* Raw JSON text of a setting, NULL when unset. Caller frees. */
char *db_setting_raw(struct db *db, const char *key);
int db_setting_put(struct db *db, const char *key, const char *json);

/* Apply retention settings; returns the number of clips removed. */
int db_retention(struct db *db, int64_t now);
/* Remove blob files that no format references. Returns files removed. */
int db_blob_gc(struct db *db);
cJSON *db_stats(struct db *db);

/* Run fn inside BEGIN IMMEDIATE ... COMMIT (ROLLBACK when it returns != 0). */
int db_tx(struct db *db, int (*fn)(struct db *db, void *ctx), void *ctx);

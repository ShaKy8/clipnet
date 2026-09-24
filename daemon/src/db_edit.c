/*
 * Changing clips and groups: everything the user does to their history on
 * purpose (as opposed to capture and retention, in db.c).
 *
 * Every function returns 0 on success and -1 on failure with *err set to a
 * short user-facing message.
 */
#include <stdlib.h>
#include <string.h>

#include "blob.h"
#include "cJSON.h"
#include "db.h"
#include "db_internal.h"
#include "util.h"

#define NAME_MAX_CHARS 200

static int step_done(struct db *db, sqlite3_stmt *st, const char **err, const char *what)
{
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc == SQLITE_DONE) return 0;
  if (rc == SQLITE_CONSTRAINT) {
    *err = what;
    return -1;
  }
  log_err("sql: %s", sqlite3_errmsg(db->h));
  *err = "database error";
  return -1;
}

static bool clip_exists(struct db *db, int64_t id, const char **err)
{
  if (db_exists(db, id)) return true;
  *err = "no such clip";
  return false;
}

/* Trimmed copy of s, NULL when empty after trimming. */
static char *trimmed(const char *s)
{
  if (!s) return NULL;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  return n ? xstrndup(s, n) : NULL;
}

/* ---- clips ------------------------------------------------------------- */

int db_clip_set_title(struct db *db, int64_t id, const char *title, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  char *t = trimmed(title);
  if (t && !utf8_valid((const uint8_t *)t, strlen(t))) { free(t); *err = "title is not valid UTF-8"; return -1; }
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET title = ? WHERE id = ?");
  if (!st) { free(t); *err = "database error"; return -1; }
  sqlite3_bind_text(st, 1, t, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  free(t);
  return step_done(db, st, err, "could not set the title");
}

int db_clip_set_quick_paste(struct db *db, int64_t id, const char *word, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  char *w = trimmed(word);
  if (w && (strchr(w, ' ') || strchr(w, '\t'))) { free(w); *err = "a quick-paste word cannot contain spaces"; return -1; }
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET quick_paste = ? WHERE id = ?");
  if (!st) { free(w); *err = "database error"; return -1; }
  sqlite3_bind_text(st, 1, w, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  free(w);
  return step_done(db, st, err, "another clip already uses that quick-paste word");
}

int db_clip_set_locked(struct db *db, int64_t id, bool locked, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET locked = ? WHERE id = ?");
  if (!st) { *err = "database error"; return -1; }
  sqlite3_bind_int(st, 1, locked);
  sqlite3_bind_int64(st, 2, id);
  return step_done(db, st, err, "could not change the lock");
}

/* Sticky clips are ordered by sticky_order ascending, above everything else.
 * "top" goes before the first sticky clip, "bottom" after the last (Ditto's
 * "make top sticky" and "make last sticky"). */
int db_clip_set_sticky(struct db *db, int64_t id, enum sticky_where where, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  const char *sql = where == STICKY_TOP
                      ? "UPDATE clips SET sticky_order = (SELECT coalesce(min(sticky_order), 1) - 1 FROM clips) WHERE id = ?"
                    : where == STICKY_BOTTOM
                      ? "UPDATE clips SET sticky_order = (SELECT coalesce(max(sticky_order), -1) + 1 FROM clips) WHERE id = ?"
                      : "UPDATE clips SET sticky_order = NULL WHERE id = ?";
  sqlite3_stmt *st = dbi_prep(db, sql);
  if (!st) { *err = "database error"; return -1; }
  sqlite3_bind_int64(st, 1, id);
  return step_done(db, st, err, "could not change sticky");
}

struct reorder_ctx {
  const int64_t *ids;
  size_t n;
  const char **err;
};

static int reorder_tx(struct db *db, void *vctx)
{
  struct reorder_ctx *c = vctx;
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET sticky_order = ? WHERE id = ? AND sticky_order IS NOT NULL");
  if (!st) { *c->err = "database error"; return -1; }
  for (size_t i = 0; i < c->n; i++) {
    sqlite3_reset(st);
    sqlite3_bind_double(st, 1, (double)i);
    sqlite3_bind_int64(st, 2, c->ids[i]);
    if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); *c->err = "database error"; return -1; }
  }
  sqlite3_finalize(st);
  return 0;
}

int db_clip_reorder_sticky(struct db *db, const int64_t *ids, size_t n, const char **err)
{
  struct reorder_ctx c = { ids, n, err };
  return db_tx(db, reorder_tx, &c) ? -1 : 0;
}

int db_clip_move(struct db *db, int64_t id, int64_t group_id, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  if (group_id && !db_group_exists(db, group_id)) { *err = "no such group"; return -1; }
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET group_id = ? WHERE id = ?");
  if (!st) { *err = "database error"; return -1; }
  if (group_id) sqlite3_bind_int64(st, 1, group_id);
  else sqlite3_bind_null(st, 1);
  sqlite3_bind_int64(st, 2, id);
  return step_done(db, st, err, "could not move the clip");
}

struct set_text_ctx {
  int64_t id;
  const struct clip_in *in;
  uint8_t hash[32];
  struct derived d;
  char **old_blobs;
  size_t n_old;
  const char **err;
};

static int set_text_tx(struct db *db, void *vctx)
{
  struct set_text_ctx *c = vctx;
  sqlite3_stmt *st = dbi_prep(db, "SELECT id FROM clips WHERE content_hash = ? AND id != ?");
  if (!st) return -1;
  sqlite3_bind_blob(st, 1, c->hash, 32, SQLITE_STATIC);
  sqlite3_bind_int64(st, 2, c->id);
  bool dup = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (dup) { *c->err = "another clip already has exactly this text"; return -1; }

  st = dbi_prep(db, "SELECT DISTINCT blob FROM clip_formats WHERE clip_id = ? AND blob IS NOT NULL");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    c->old_blobs = xrealloc(c->old_blobs, (c->n_old + 1) * sizeof *c->old_blobs);
    c->old_blobs[c->n_old++] = xstrdup((const char *)sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);

  st = dbi_prep(db, "DELETE FROM clip_formats WHERE clip_id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return -1; }
  sqlite3_finalize(st);
  if (dbi_insert_formats(db, c->id, c->in->fmts, c->in->n_fmts) < 0) return -1;

  st = dbi_prep(db, "UPDATE clips SET kind = ?, preview = ?, plain_text = ?, content_hash = ?, total_size = ?,"
                    " flags = ? WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, kind_name(c->d.kind), -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, c->d.preview, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, c->d.plain, -1, SQLITE_STATIC);
  sqlite3_bind_blob(st, 4, c->hash, 32, SQLITE_STATIC);
  sqlite3_bind_int64(st, 5, (int64_t)c->in->fmts[0].len);
  sqlite3_bind_int(st, 6, c->d.flags);
  sqlite3_bind_int64(st, 7, c->id);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  return rc;
}

/* Replace a clip's content with plain text (the clip editor). Rich formats
 * are dropped: the edited text no longer matches them. Title, group, sticky,
 * lock, hotkey and quick-paste word stay with the clip. */
int db_clip_set_text(struct db *db, int64_t id, const char *text, const char **err)
{
  if (!clip_exists(db, id, err)) return -1;
  if (!text || !*text) { *err = "the text is empty"; return -1; }
  if (!utf8_valid((const uint8_t *)text, strlen(text))) { *err = "the text is not valid UTF-8"; return -1; }
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)text, strlen(text), mime_text_default_aliases,
                      mime_text_default_alias_count };
  struct clip_in in = { .fmts = &f, .n_fmts = 1 };
  struct set_text_ctx c = { .id = id, .in = &in, .err = err };
  dbi_content_hash(&in, c.hash);
  dbi_derive(&in, &c.d);
  *err = NULL;
  int rc = db_tx(db, set_text_tx, &c);
  if (rc && !*err) *err = "database error";
  for (size_t i = 0; i < c.n_old; i++) {
    if (rc == 0 && !dbi_blob_referenced(db, c.old_blobs[i])) blob_unlink(db->blob_dir, c.old_blobs[i]);
    free(c.old_blobs[i]);
  }
  free(c.old_blobs);
  free(c.d.plain);
  free(c.d.preview);
  return rc ? -1 : 0;
}

/* ---- groups ------------------------------------------------------------ */

bool db_group_exists(struct db *db, int64_t id)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT 1 FROM groups WHERE id = ?");
  if (!st) return false;
  sqlite3_bind_int64(st, 1, id);
  bool r = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return r;
}

static char *valid_name(const char *name, const char **err)
{
  char *n = trimmed(name);
  if (!n) { *err = "a group needs a name"; return NULL; }
  if (!utf8_valid((const uint8_t *)n, strlen(n))) { free(n); *err = "the name is not valid UTF-8"; return NULL; }
  size_t cut = utf8_prefix(n, strlen(n), NAME_MAX_CHARS);
  n[cut] = 0;
  return n;
}

int64_t db_group_create(struct db *db, const char *name, int64_t parent_id, const char **err)
{
  if (parent_id && !db_group_exists(db, parent_id)) { *err = "no such parent group"; return -1; }
  char *n = valid_name(name, err);
  if (!n) return -1;
  char uuid[37];
  uuid_v4(uuid);
  sqlite3_stmt *st = dbi_prep(db, "INSERT INTO groups(uuid, parent_id, name, sort_order, created_at)"
                                  " VALUES (?, ?, ?, (SELECT coalesce(max(sort_order), 0) + 1 FROM groups), ?)");
  if (!st) { free(n); *err = "database error"; return -1; }
  sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
  if (parent_id) sqlite3_bind_int64(st, 2, parent_id);
  else sqlite3_bind_null(st, 2);
  sqlite3_bind_text(st, 3, n, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, now_ms());
  free(n);
  if (step_done(db, st, err, "a group with that name already exists here") < 0) return -1;
  return sqlite3_last_insert_rowid(db->h);
}

int db_group_rename(struct db *db, int64_t id, const char *name, const char **err)
{
  if (!db_group_exists(db, id)) { *err = "no such group"; return -1; }
  char *n = valid_name(name, err);
  if (!n) return -1;
  sqlite3_stmt *st = dbi_prep(db, "UPDATE groups SET name = ? WHERE id = ?");
  if (!st) { free(n); *err = "database error"; return -1; }
  sqlite3_bind_text(st, 1, n, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  free(n);
  return step_done(db, st, err, "a group with that name already exists here");
}

/* Is `maybe_descendant` the group itself or somewhere below it? */
static bool within(struct db *db, int64_t group, int64_t maybe_descendant)
{
  sqlite3_stmt *st = dbi_prep(db, "WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT g.id FROM groups g"
                                  " JOIN sub ON g.parent_id = sub.id) SELECT 1 FROM sub WHERE id = ?");
  if (!st) return true; /* when unsure, refuse the move */
  sqlite3_bind_int64(st, 1, group);
  sqlite3_bind_int64(st, 2, maybe_descendant);
  bool r = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return r;
}

int db_group_move(struct db *db, int64_t id, int64_t parent_id, const char **err)
{
  if (!db_group_exists(db, id)) { *err = "no such group"; return -1; }
  if (parent_id && !db_group_exists(db, parent_id)) { *err = "no such parent group"; return -1; }
  if (parent_id && within(db, id, parent_id)) { *err = "a group cannot go inside itself"; return -1; }
  sqlite3_stmt *st = dbi_prep(db, "UPDATE groups SET parent_id = ? WHERE id = ?");
  if (!st) { *err = "database error"; return -1; }
  if (parent_id) sqlite3_bind_int64(st, 1, parent_id);
  else sqlite3_bind_null(st, 1);
  sqlite3_bind_int64(st, 2, id);
  return step_done(db, st, err, "a group with that name already exists there");
}

struct gdel_ctx {
  int64_t id;
  bool cascade;
  int64_t *clips;
  size_t n_clips;
};

static int gdel_tx(struct db *db, void *vctx)
{
  struct gdel_ctx *c = vctx;
  const char *tree = "WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT g.id FROM groups g JOIN sub ON g.parent_id = sub.id) ";
  char *sql = xasprintf("%sSELECT id FROM clips WHERE group_id IN (SELECT id FROM sub)", tree);
  sqlite3_stmt *st = dbi_prep(db, sql);
  free(sql);
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    c->clips = xrealloc(c->clips, (c->n_clips + 1) * sizeof *c->clips);
    c->clips[c->n_clips++] = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  if (c->cascade) {
    sql = xasprintf("%sDELETE FROM clips WHERE group_id IN (SELECT id FROM sub)", tree);
    st = dbi_prep(db, sql);
    free(sql);
    if (!st) return -1;
    sqlite3_bind_int64(st, 1, c->id);
    if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return -1; }
    sqlite3_finalize(st);
  }
  /* Subgroups go with it (ON DELETE CASCADE); clips left behind fall back to
   * plain history (ON DELETE SET NULL). */
  st = dbi_prep(db, "DELETE FROM groups WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  return rc;
}

int db_group_delete(struct db *db, int64_t id, bool cascade, int64_t **affected, size_t *n_affected, const char **err)
{
  if (!db_group_exists(db, id)) { *err = "no such group"; return -1; }
  struct gdel_ctx c = { .id = id, .cascade = cascade };
  if (db_tx(db, gdel_tx, &c)) {
    free(c.clips);
    *err = "database error";
    return -1;
  }
  if (cascade) db_blob_gc(db);
  *affected = c.clips;
  *n_affected = c.n_clips;
  return 0;
}

cJSON *db_groups_list(struct db *db)
{
  cJSON *arr = cJSON_CreateArray();
  sqlite3_stmt *st = dbi_prep(db, "SELECT g.id, g.uuid, g.parent_id, g.name, g.sort_order,"
                                  " (SELECT count(*) FROM clips c WHERE c.group_id = g.id)"
                                  " FROM groups g ORDER BY g.sort_order, g.name");
  if (!st) return arr;
  while (sqlite3_step(st) == SQLITE_ROW) {
    cJSON *g = cJSON_CreateObject();
    cJSON_AddNumberToObject(g, "id", (double)sqlite3_column_int64(st, 0));
    cJSON_AddStringToObject(g, "uuid", (const char *)sqlite3_column_text(st, 1));
    if (sqlite3_column_type(st, 2) == SQLITE_NULL) cJSON_AddNullToObject(g, "parent_id");
    else cJSON_AddNumberToObject(g, "parent_id", (double)sqlite3_column_int64(st, 2));
    cJSON_AddStringToObject(g, "name", (const char *)sqlite3_column_text(st, 3));
    cJSON_AddNumberToObject(g, "count", (double)sqlite3_column_int64(st, 5));
    cJSON_AddItemToArray(arr, g);
  }
  sqlite3_finalize(st);
  return arr;
}

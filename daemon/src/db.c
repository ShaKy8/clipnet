#include "db.h"
#include "db_internal.h"

#include <dirent.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blob.h"
#include "cJSON.h"
#include "imgmeta.h"
#include "mime.h"
#include "schema.h"
#include "settings.h"
#include "sha256.h"
#include "textutil.h"
#include "util.h"

/* Formats up to this size sit inline in SQLite; images never do, so the UI
 * can load them by path. */
#define INLINE_MAX (64 * 1024)
/* Searchable/derived text is capped; the full payload stays in its format. */
#define PLAIN_TEXT_MAX_CHARS 1000000
#define PREVIEW_CHARS 300


sqlite3 *db_handle(struct db *db) { return db->h; }
const char *db_dir(struct db *db) { return db->dir; }
const char *db_blob_dir(struct db *db) { return db->blob_dir; }

sqlite3_stmt *dbi_prep(struct db *db, const char *sql)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK) {
    log_err("sql prepare: %s", sqlite3_errmsg(db->h));
    return NULL;
  }
  return st;
}

int dbi_exec(struct db *db, const char *sql)
{
  char *err = NULL;
  if (sqlite3_exec(db->h, sql, NULL, NULL, &err) != SQLITE_OK) {
    log_err("sql: %s", err ? err : "?");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

struct db *db_open(const char *dir)
{
  if (mkdir_p(dir, 0700) < 0) {
    log_err("create %s: %m", dir);
    return NULL;
  }
  struct db *db = xcalloc(1, sizeof *db);
  db->dir = xstrdup(dir);
  db->blob_dir = xasprintf("%s/blobs", dir);
  mkdir_p(db->blob_dir, 0700);
  char *path = xasprintf("%s/clipnet.db", dir);
  int rc = sqlite3_open_v2(path, &db->h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc == SQLITE_OK) chmod(path, 0600);
  free(path);
  if (rc != SQLITE_OK) {
    log_err("open database: %s", db->h ? sqlite3_errmsg(db->h) : sqlite3_errstr(rc));
    db_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db->h, 2000);
  if (dbi_exec(db, "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON;"
               "PRAGMA secure_delete = ON; PRAGMA temp_store = MEMORY;") < 0 ||
      schema_migrate(db->h) < 0) {
    db_close(db);
    return NULL;
  }
  return db;
}

void db_close(struct db *db)
{
  if (!db) return;
  if (db->h) {
    sqlite3_exec(db->h, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
    sqlite3_close_v2(db->h);
  }
  free(db->dir);
  free(db->blob_dir);
  free(db);
}

int db_tx(struct db *db, int (*fn)(struct db *db, void *ctx), void *ctx)
{
  if (dbi_exec(db, "BEGIN IMMEDIATE") < 0) return -1;
  int rc = fn(db, ctx);
  if (rc == 0 && dbi_exec(db, "COMMIT") == 0) return 0;
  dbi_exec(db, "ROLLBACK");
  return rc ? rc : -1;
}

/* ---- adding clips ---------------------------------------------------- */

static const struct fmt_in *sorted_fmts_cmp_base;
static int cmp_fmt_idx(const void *a, const void *b)
{
  const struct fmt_in *f = sorted_fmts_cmp_base;
  return strcmp(f[*(const size_t *)a].mime, f[*(const size_t *)b].mime);
}

/* Identity of a clip: every (mime, bytes) pair, in mime order, so the same
 * content offered in a different order is still the same clip. */
void dbi_content_hash(const struct clip_in *in, uint8_t out[32])
{
  size_t *idx = xmalloc(in->n_fmts * sizeof *idx);
  for (size_t i = 0; i < in->n_fmts; i++) idx[i] = i;
  sorted_fmts_cmp_base = in->fmts;
  qsort(idx, in->n_fmts, sizeof *idx, cmp_fmt_idx);
  struct sha256 s;
  sha256_init(&s);
  for (size_t k = 0; k < in->n_fmts; k++) {
    const struct fmt_in *f = &in->fmts[idx[k]];
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)((uint64_t)f->len >> (8 * i));
    sha256_update(&s, f->mime, strlen(f->mime) + 1);
    sha256_update(&s, len, 8);
    sha256_update(&s, f->data, f->len);
  }
  sha256_final(&s, out);
  free(idx);
}

static const struct fmt_in *find_fmt(const struct clip_in *in, const char *mime)
{
  for (size_t i = 0; i < in->n_fmts; i++)
    if (!strcmp(in->fmts[i].mime, mime)) return &in->fmts[i];
  return NULL;
}

static char *valid_text(const uint8_t *p, size_t n, size_t *out_len)
{
  if (utf8_valid(p, n)) {
    size_t cut = utf8_prefix((const char *)p, n, PLAIN_TEXT_MAX_CHARS);
    if (out_len) *out_len = cut;
    return xstrndup((const char *)p, cut);
  }
  return latin1_to_utf8(p, MIN(n, (size_t)PLAIN_TEXT_MAX_CHARS), out_len);
}


void dbi_derive(const struct clip_in *in, struct derived *d)
{
  const char **mimes = xmalloc((in->n_fmts ? in->n_fmts : 1) * sizeof *mimes);
  for (size_t i = 0; i < in->n_fmts; i++) mimes[i] = in->fmts[i].mime;
  d->kind = mime_classify(mimes, in->n_fmts);
  free(mimes);

  const struct fmt_in *text = find_fmt(in, MIME_TEXT);
  const struct fmt_in *html = find_fmt(in, MIME_HTML);
  const struct fmt_in *uris = find_fmt(in, MIME_URI_LIST);
  d->plain = NULL;
  if (d->kind == KIND_FILES && uris) {
    char *t = valid_text(uris->data, uris->len, NULL);
    d->plain = uri_list_to_paths(t, strlen(t));
    free(t);
  } else if (text && !(html && html->len == text->len && !memcmp(html->data, text->data, text->len))) {
    d->plain = valid_text(text->data, text->len, NULL);
  } else if (html) {
    /* Either no plain text, or a source that offered its HTML markup under
     * the plain-text names too (wl-copy -t text/html does): the visible text
     * of the HTML is what search, preview and plain paste should see. */
    if (text) d->flags |= CLIP_FLAG_TEXT_IS_HTML;
    char *t = valid_text(html->data, html->len, NULL);
    d->plain = html_to_text(t, strlen(t));
    free(t);
  }

  if (d->kind == KIND_IMAGE) {
    for (size_t i = 0; i < in->n_fmts; i++) {
      const struct fmt_in *f = &in->fmts[i];
      if (!mime_is_image(f->mime)) continue;
      struct img_meta m;
      char sz[32];
      human_size(f->len, sz);
      if (img_meta_parse(f->data, f->len, &m) && m.width)
        d->preview = xasprintf("[%s %u×%u · %s]", m.format, m.width, m.height, sz);
      else
        d->preview = xasprintf("[%s · %s]", f->mime + 6, sz);
      return;
    }
  }
  if (d->plain && d->plain[0]) {
    d->preview = text_preview(d->plain, strlen(d->plain), PREVIEW_CHARS);
    if (d->preview[0]) return;
    free(d->preview);
  }
  /* No readable text: name the richest format instead. */
  size_t total = 0;
  for (size_t i = 0; i < in->n_fmts; i++) total += in->fmts[i].len;
  char sz[32];
  human_size(total, sz);
  if (d->plain && !d->plain[0] && text) d->preview = xasprintf("[whitespace · %s]", sz);
  else d->preview = xasprintf("[%s · %s]", in->n_fmts ? in->fmts[0].mime : "empty", sz);
}

struct add_ctx {
  const struct clip_in *in;
  uint8_t hash[32];
  struct derived d;
  int64_t id;
  enum add_result result;
};

static char *aliases_json(const struct fmt_in *f)
{
  if (!f->n_aliases) return NULL;
  cJSON *a = cJSON_CreateArray();
  for (size_t i = 0; i < f->n_aliases; i++) cJSON_AddItemToArray(a, cJSON_CreateString(f->aliases[i]));
  char *s = cJSON_PrintUnformatted(a);
  cJSON_Delete(a);
  return s;
}

/* Write a clip's formats: small ones inline, images and big ones as blobs. */
int dbi_insert_formats(struct db *db, int64_t clip_id, const struct fmt_in *fmts, size_t n)
{
  sqlite3_stmt *st = dbi_prep(db, "INSERT INTO clip_formats(clip_id, ord, mime, raw_name, aliases, size, hash, data, blob)"
                              " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  if (!st) return -1;
  for (size_t i = 0; i < n; i++) {
    const struct fmt_in *f = &fmts[i];
    uint8_t h[32];
    sha256(f->data, f->len, h);
    char name[68];
    bool as_blob = mime_is_image(f->mime) || f->len > INLINE_MAX;
    if (as_blob) {
      blob_name(h, name);
      if (blob_put(db->blob_dir, name, f->data, f->len) < 0) { sqlite3_finalize(st); return -1; }
    }
    char *aj = aliases_json(f);
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, clip_id);
    sqlite3_bind_int(st, 2, (int)i);
    sqlite3_bind_text(st, 3, f->mime, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, f->n_aliases ? f->aliases[0] : f->mime, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, aj, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (int64_t)f->len);
    sqlite3_bind_blob(st, 7, h, 32, SQLITE_TRANSIENT);
    if (as_blob) {
      sqlite3_bind_null(st, 8);
      sqlite3_bind_text(st, 9, name, -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_blob64(st, 8, f->len ? f->data : (const void *)"", f->len, SQLITE_STATIC);
      sqlite3_bind_null(st, 9);
    }
    int rc = sqlite3_step(st);
    free(aj);
    if (rc != SQLITE_DONE) {
      log_err("insert format %s: %s", f->mime, sqlite3_errmsg(db->h));
      sqlite3_finalize(st);
      return -1;
    }
  }
  sqlite3_finalize(st);
  return 0;
}

static int add_tx(struct db *db, void *vctx)
{
  struct add_ctx *c = vctx;
  const struct clip_in *in = c->in;
  int64_t now = now_ms();
  int64_t created = in->created_at ? in->created_at : now;
  int64_t used = in->last_used_at ? in->last_used_at : created;

  sqlite3_stmt *st = dbi_prep(db, "SELECT id FROM clips WHERE content_hash = ?");
  if (!st) return -1;
  sqlite3_bind_blob(st, 1, c->hash, 32, SQLITE_STATIC);
  int64_t existing = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  if (existing) {
    st = dbi_prep(db, "UPDATE clips SET last_used_at = max(last_used_at, ?),"
                  " source_app = coalesce(?, source_app) WHERE id = ?");
    if (!st) return -1;
    sqlite3_bind_int64(st, 1, used);
    sqlite3_bind_text(st, 2, in->source_app, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, existing);
    int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    c->id = existing;
    c->result = ADD_DUP;
    return rc;
  }

  char uuid[37];
  if (in->uuid) snprintf(uuid, sizeof uuid, "%s", in->uuid);
  else uuid_v4(uuid);
  size_t total = 0;
  for (size_t i = 0; i < in->n_fmts; i++) total += in->fmts[i].len;

  st = dbi_prep(db, "INSERT INTO clips(uuid, created_at, last_used_at, kind, title, preview, plain_text,"
                " content_hash, total_size, source_app, source_title, group_id, flags)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 2, created);
  sqlite3_bind_int64(st, 3, used);
  sqlite3_bind_text(st, 4, kind_name(c->d.kind), -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 5, in->title, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 6, c->d.preview, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 7, c->d.plain, -1, SQLITE_STATIC);
  sqlite3_bind_blob(st, 8, c->hash, 32, SQLITE_STATIC);
  sqlite3_bind_int64(st, 9, (int64_t)total);
  sqlite3_bind_text(st, 10, in->source_app, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 11, in->source_title, -1, SQLITE_STATIC);
  if (in->group_id) sqlite3_bind_int64(st, 12, in->group_id);
  else sqlite3_bind_null(st, 12);
  sqlite3_bind_int(st, 13, c->d.flags);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    log_err("insert clip: %s", sqlite3_errmsg(db->h));
    return -1;
  }
  c->id = sqlite3_last_insert_rowid(db->h);

  if (dbi_insert_formats(db, c->id, in->fmts, in->n_fmts) < 0) return -1;
  c->result = ADD_NEW;
  return 0;
}

enum add_result db_add_clip(struct db *db, const struct clip_in *in, int64_t *id)
{
  if (!in->n_fmts) return ADD_ERR;
  struct add_ctx c = { .in = in, .result = ADD_ERR };
  dbi_content_hash(in, c.hash);
  dbi_derive(in, &c.d);
  int rc = db_tx(db, add_tx, &c);
  free(c.d.plain);
  free(c.d.preview);
  if (rc) return ADD_ERR;
  if (id) *id = c.id;
  return c.result;
}

/* ---- reading --------------------------------------------------------- */

#define ROW_COLS \
  "c.id, c.uuid, c.kind, c.preview, c.title, c.created_at, c.last_used_at, c.paste_count," \
  " c.source_app, c.sticky_order, c.locked, c.group_id, c.quick_paste, c.total_size," \
  " (SELECT group_concat(mime, char(10) ORDER BY ord) FROM clip_formats WHERE clip_id = c.id)," \
  " (SELECT blob FROM clip_formats WHERE clip_id = c.id AND blob IS NOT NULL AND mime LIKE 'image/%'" \
  "  ORDER BY mime = 'image/png' DESC LIMIT 1)," \
  " c.flags, c.source_title," \
  " (SELECT accel FROM hotkeys h WHERE h.action = 'paste_clip' AND h.arg = c.uuid LIMIT 1)"

static void add_text_or_null(cJSON *o, const char *k, sqlite3_stmt *st, int col)
{
  const char *s = (const char *)sqlite3_column_text(st, col);
  if (s) cJSON_AddStringToObject(o, k, s);
  else cJSON_AddNullToObject(o, k);
}

static cJSON *row_json(struct db *db, sqlite3_stmt *st)
{
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "id", (double)sqlite3_column_int64(st, 0));
  add_text_or_null(o, "uuid", st, 1);
  add_text_or_null(o, "kind", st, 2);
  add_text_or_null(o, "preview", st, 3);
  add_text_or_null(o, "title", st, 4);
  cJSON_AddNumberToObject(o, "created_at", (double)sqlite3_column_int64(st, 5));
  cJSON_AddNumberToObject(o, "last_used_at", (double)sqlite3_column_int64(st, 6));
  cJSON_AddNumberToObject(o, "paste_count", sqlite3_column_int(st, 7));
  add_text_or_null(o, "source_app", st, 8);
  cJSON_AddBoolToObject(o, "sticky", sqlite3_column_type(st, 9) != SQLITE_NULL);
  cJSON_AddBoolToObject(o, "locked", sqlite3_column_int(st, 10) != 0);
  if (sqlite3_column_type(st, 11) == SQLITE_NULL) cJSON_AddNullToObject(o, "group_id");
  else cJSON_AddNumberToObject(o, "group_id", (double)sqlite3_column_int64(st, 11));
  add_text_or_null(o, "quick_paste", st, 12);
  cJSON_AddNumberToObject(o, "size", (double)sqlite3_column_int64(st, 13));
  cJSON *mimes = cJSON_AddArrayToObject(o, "mimes");
  const char *ml = (const char *)sqlite3_column_text(st, 14);
  while (ml && *ml) {
    const char *nl = strchr(ml, '\n');
    size_t len = nl ? (size_t)(nl - ml) : strlen(ml);
    char *m = xstrndup(ml, len);
    cJSON_AddItemToArray(mimes, cJSON_CreateString(m));
    free(m);
    ml = nl ? nl + 1 : NULL;
  }
  const char *blob = (const char *)sqlite3_column_text(st, 15);
  if (blob) {
    char *p = blob_path(db->blob_dir, blob);
    cJSON_AddStringToObject(o, "image", p);
    free(p);
  } else {
    cJSON_AddNullToObject(o, "image");
  }
  cJSON_AddNumberToObject(o, "flags", sqlite3_column_int(st, 16));
  add_text_or_null(o, "source_title", st, 17);
  add_text_or_null(o, "hotkey", st, 18);
  return o;
}

/* WHERE fragment and its text parameters for a list scope + search. */
struct where {
  struct buf sql;
  char *params[64];
  size_t n;
};

static void where_param(struct where *w, char *owned)
{
  if (w->n < ARRAY_LEN(w->params)) w->params[w->n++] = owned;
  else free(owned);
}

static void where_free(struct where *w)
{
  for (size_t i = 0; i < w->n; i++) free(w->params[i]);
  buf_free(&w->sql);
}

static void build_where(struct db *db, const struct list_query *q, struct where *w)
{
  buf_append_str(&w->sql, " WHERE 1");
  if (!q->all_groups) {
    if (q->group_id) {
      char *id = xasprintf("%lld", (long long)q->group_id);
      buf_append_str(&w->sql, " AND c.group_id = CAST(? AS INTEGER)");
      where_param(w, id);
    } else if (!setting_bool(db, "show_grouped_in_history")) {
      buf_append_str(&w->sql, " AND c.group_id IS NULL");
    }
  }
  if (!q->query || !q->query[0]) return;

  /* Terms are ANDed. Three or more characters go through the trigram index;
   * shorter ones cannot (a trigram needs three) and fall back to LIKE. */
  struct buf match = { 0 };
  const char *s = q->query;
  while (*s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
    if (e == s) break;
    size_t len = (size_t)(e - s);
    size_t chars = 0;
    for (size_t i = 0; i < len; i++)
      if (((unsigned char)s[i] & 0xC0) != 0x80) chars++;
    if (chars >= 3) {
      if (match.len) buf_append_str(&match, " AND ");
      buf_append(&match, "\"", 1);
      for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') buf_append(&match, "\"\"", 2);
        else buf_append(&match, s + i, 1);
      }
      buf_append(&match, "\"", 1);
    } else {
      struct buf pat = { 0 };
      buf_append(&pat, "%", 1);
      for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' || s[i] == '_' || s[i] == '\\') buf_append(&pat, "\\", 1);
        buf_append(&pat, s + i, 1);
      }
      buf_append(&pat, "%", 1);
      buf_append_str(&w->sql, " AND (c.plain_text LIKE ? ESCAPE '\\' OR c.title LIKE ? ESCAPE '\\'"
                              " OR c.quick_paste LIKE ? ESCAPE '\\')");
      char *p = buf_steal(&pat, NULL);
      where_param(w, xstrdup(p));
      where_param(w, xstrdup(p));
      where_param(w, p);
    }
    s = e;
  }
  if (match.len) {
    buf_append_str(&w->sql, " AND (c.id IN (SELECT rowid FROM clips_fts WHERE clips_fts MATCH ?)"
                            " OR c.quick_paste = ?)");
    where_param(w, buf_steal(&match, NULL));
    where_param(w, xstrdup(q->query));
  } else {
    buf_free(&match);
  }
}

static void bind_where(sqlite3_stmt *st, const struct where *w)
{
  for (size_t i = 0; i < w->n; i++) sqlite3_bind_text(st, (int)i + 1, w->params[i], -1, SQLITE_STATIC);
}

#define ORDER_HISTORY " ORDER BY (c.sticky_order IS NULL), c.sticky_order, c.last_used_at DESC, c.id DESC"

cJSON *db_list(struct db *db, const struct list_query *q)
{
  struct where w = { 0 };
  build_where(db, q, &w);
  cJSON *out = cJSON_CreateObject();
  cJSON *rows = cJSON_AddArrayToObject(out, "rows");

  struct buf sql = { 0 };
  /* Not buf_printf: ROW_COLS holds a LIKE pattern with a literal '%'. */
  buf_append_str(&sql, "SELECT " ROW_COLS " FROM clips c");
  buf_append_str(&sql, (char *)w.sql.data);
  /* An exact quick-paste word jumps ahead of everything (Ditto behaviour). */
  if (q->query && q->query[0]) buf_append_str(&sql, " ORDER BY (c.quick_paste IS ?) DESC, (c.sticky_order IS NULL),"
                                                    " c.sticky_order, c.last_used_at DESC, c.id DESC");
  else buf_append_str(&sql, ORDER_HISTORY);
  buf_append_str(&sql, " LIMIT ? OFFSET ?");
  sqlite3_stmt *st = dbi_prep(db, (char *)sql.data);
  buf_free(&sql);
  if (st) {
    bind_where(st, &w);
    int k = (int)w.n + 1;
    if (q->query && q->query[0]) sqlite3_bind_text(st, k++, q->query, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, k++, q->limit > 0 ? q->limit : 200);
    sqlite3_bind_int(st, k, q->offset > 0 ? q->offset : 0);
    while (sqlite3_step(st) == SQLITE_ROW) cJSON_AddItemToArray(rows, row_json(db, st));
    sqlite3_finalize(st);
  }

  buf_printf(&sql, "SELECT count(*) FROM clips c%s", (char *)w.sql.data);
  st = dbi_prep(db, (char *)sql.data);
  buf_free(&sql);
  int64_t total = 0;
  if (st) {
    bind_where(st, &w);
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  cJSON_AddNumberToObject(out, "total", (double)total);
  where_free(&w);
  return out;
}

int64_t db_id_at_position(struct db *db, int pos)
{
  struct list_query q = { 0 };
  struct where w = { 0 };
  build_where(db, &q, &w);
  struct buf sql = { 0 };
  buf_printf(&sql, "SELECT c.id FROM clips c%s" ORDER_HISTORY " LIMIT 1 OFFSET ?", (char *)w.sql.data);
  sqlite3_stmt *st = dbi_prep(db, (char *)sql.data);
  buf_free(&sql);
  int64_t id = 0;
  if (st) {
    bind_where(st, &w);
    sqlite3_bind_int(st, (int)w.n + 1, pos);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  where_free(&w);
  return id;
}

cJSON *db_row(struct db *db, int64_t id)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT " ROW_COLS " FROM clips c WHERE c.id = ?");
  if (!st) return NULL;
  sqlite3_bind_int64(st, 1, id);
  cJSON *o = sqlite3_step(st) == SQLITE_ROW ? row_json(db, st) : NULL;
  sqlite3_finalize(st);
  return o;
}

bool db_exists(struct db *db, int64_t id)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT 1 FROM clips WHERE id = ?");
  if (!st) return false;
  sqlite3_bind_int64(st, 1, id);
  bool r = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return r;
}

int64_t db_clip_id_by_uuid(struct db *db, const char *uuid)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT id FROM clips WHERE uuid = ?");
  if (!st) return 0;
  sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
  int64_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  sqlite3_finalize(st);
  return id;
}

int db_clip_uuid(struct db *db, int64_t id, char out[37])
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT uuid FROM clips WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, id);
  int rc = -1;
  if (sqlite3_step(st) == SQLITE_ROW) {
    snprintf(out, 37, "%s", (const char *)sqlite3_column_text(st, 0));
    rc = 0;
  }
  sqlite3_finalize(st);
  return rc;
}

/* Copy (or map) one format's bytes out of a clip_formats row. */
static int format_bytes(struct db *db, sqlite3_stmt *st, int data_col, int blob_col, struct fmt_out *f)
{
  const char *blob = (const char *)sqlite3_column_text(st, blob_col);
  if (blob) {
    size_t len = 0;
    void *p = blob_map(db->blob_dir, blob, &len);
    if (!p) {
      log_err("blob %s missing", blob);
      return -1;
    }
    if (len) {
      f->data = p;
      f->map_len = len;
    } else {
      f->data = xcalloc(1, 1); /* blob_map hands back a static "" for empty files */
      f->map_len = 0;
    }
    f->len = len;
    return 0;
  }
  size_t len = (size_t)sqlite3_column_bytes(st, data_col);
  const void *p = sqlite3_column_blob(st, data_col);
  uint8_t *copy = xmalloc(len + 1);
  if (len) memcpy(copy, p, len);
  copy[len] = 0;
  f->data = copy;
  f->len = len;
  f->map_len = 0;
  return 0;
}

static void fmt_out_release(struct fmt_out *f)
{
  if (f->map_len) blob_unmap((void *)f->data, f->map_len);
  else free((void *)f->data);
  f->data = NULL;
}

void clip_payload_free(struct clip_payload *p)
{
  for (size_t i = 0; i < p->n_fmts; i++) {
    struct fmt_out *f = &p->fmts[i];
    free(f->mime);
    for (size_t k = 0; k < f->n_aliases; k++) free(f->aliases[k]);
    free(f->aliases);
    fmt_out_release(f);
  }
  free(p->fmts);
  memset(p, 0, sizeof *p);
}

int db_load_payload(struct db *db, int64_t id, struct clip_payload *out)
{
  memset(out, 0, sizeof *out);
  sqlite3_stmt *st = dbi_prep(db, "SELECT uuid FROM clips WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, id);
  if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
  snprintf(out->uuid, sizeof out->uuid, "%s", (const char *)sqlite3_column_text(st, 0));
  sqlite3_finalize(st);
  out->id = id;

  st = dbi_prep(db, "SELECT mime, aliases, data, blob FROM clip_formats WHERE clip_id = ? ORDER BY ord");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, id);
  size_t cap = 0;
  int rc = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (out->n_fmts == cap) {
      cap = cap ? cap * 2 : 4;
      out->fmts = xrealloc(out->fmts, cap * sizeof *out->fmts);
    }
    struct fmt_out *f = &out->fmts[out->n_fmts];
    memset(f, 0, sizeof *f);
    if (format_bytes(db, st, 2, 3, f) < 0) { rc = -1; break; }
    f->mime = xstrdup((const char *)sqlite3_column_text(st, 0));
    const char *aj = (const char *)sqlite3_column_text(st, 1);
    cJSON *a = aj ? cJSON_Parse(aj) : NULL;
    if (cJSON_IsArray(a)) {
      int n = cJSON_GetArraySize(a);
      f->aliases = xcalloc((size_t)n, sizeof *f->aliases);
      cJSON *it;
      cJSON_ArrayForEach(it, a)
        if (cJSON_IsString(it)) f->aliases[f->n_aliases++] = xstrdup(it->valuestring);
    }
    cJSON_Delete(a);
    out->n_fmts++;
  }
  sqlite3_finalize(st);
  if (rc < 0 || !out->n_fmts) {
    clip_payload_free(out);
    return -1;
  }
  return 0;
}

char *db_clip_text(struct db *db, int64_t id, size_t *len)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT f.data, f.blob, c.plain_text FROM clips c"
                              " LEFT JOIN clip_formats f ON f.clip_id = c.id AND f.mime = '" MIME_TEXT "'"
                              "  AND (c.flags & 1) = 0"
                              " WHERE c.id = ?");
  if (!st) return NULL;
  sqlite3_bind_int64(st, 1, id);
  char *out = NULL;
  if (sqlite3_step(st) == SQLITE_ROW) {
    if (sqlite3_column_type(st, 0) != SQLITE_NULL || sqlite3_column_type(st, 1) != SQLITE_NULL) {
      struct fmt_out f = { 0 };
      if (format_bytes(db, st, 0, 1, &f) == 0) {
        if (utf8_valid(f.data, f.len)) out = xstrndup((const char *)f.data, f.len);
        else out = latin1_to_utf8(f.data, f.len, NULL);
        fmt_out_release(&f);
      }
    } else if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
      out = xstrdup((const char *)sqlite3_column_text(st, 2));
    }
  }
  sqlite3_finalize(st);
  if (out && len) *len = strlen(out);
  return out;
}

cJSON *db_get(struct db *db, int64_t id)
{
  cJSON *o = db_row(db, id);
  if (!o) return NULL;
  cJSON *fmts = cJSON_AddArrayToObject(o, "formats");
  sqlite3_stmt *st = dbi_prep(db, "SELECT mime, size, aliases, blob FROM clip_formats WHERE clip_id = ? ORDER BY ord");
  if (st) {
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "mime", (const char *)sqlite3_column_text(st, 0));
      cJSON_AddNumberToObject(f, "size", (double)sqlite3_column_int64(st, 1));
      const char *aj = (const char *)sqlite3_column_text(st, 2);
      cJSON *a = aj ? cJSON_Parse(aj) : NULL;
      cJSON_AddItemToObject(f, "aliases", a ? a : cJSON_CreateArray());
      const char *blob = (const char *)sqlite3_column_text(st, 3);
      if (blob) {
        char *p = blob_path(db->blob_dir, blob);
        cJSON_AddStringToObject(f, "path", p);
        free(p);
      }
      cJSON_AddItemToArray(fmts, f);
    }
    sqlite3_finalize(st);
  }
  char *text = db_clip_text(db, id, NULL);
  if (text) cJSON_AddStringToObject(o, "text", text);
  else cJSON_AddNullToObject(o, "text");
  free(text);
  return o;
}

/* ---- changing -------------------------------------------------------- */

struct del_ctx {
  int64_t id;
  char **blobs;
  size_t n_blobs;
};

static int del_tx(struct db *db, void *vctx)
{
  struct del_ctx *c = vctx;
  sqlite3_stmt *st = dbi_prep(db, "SELECT DISTINCT blob FROM clip_formats WHERE clip_id = ? AND blob IS NOT NULL");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    c->blobs = xrealloc(c->blobs, (c->n_blobs + 1) * sizeof *c->blobs);
    c->blobs[c->n_blobs++] = xstrdup((const char *)sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  st = dbi_prep(db, "DELETE FROM clips WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, c->id);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  if (rc == 0 && sqlite3_changes(db->h) == 0) rc = 1; /* not found */
  return rc;
}

bool dbi_blob_referenced(struct db *db, const char *name)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT 1 FROM clip_formats WHERE blob = ? LIMIT 1");
  if (!st) return true; /* when unsure, keep the file */
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  bool r = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return r;
}

int db_delete(struct db *db, int64_t id)
{
  struct del_ctx c = { .id = id };
  int rc = db_tx(db, del_tx, &c);
  for (size_t i = 0; i < c.n_blobs; i++) {
    if (rc == 0 && !dbi_blob_referenced(db, c.blobs[i])) blob_unlink(db->blob_dir, c.blobs[i]);
    free(c.blobs[i]);
  }
  free(c.blobs);
  return rc;
}

int db_touch(struct db *db, int64_t id, bool pasted)
{
  sqlite3_stmt *st = dbi_prep(db, "UPDATE clips SET last_used_at = ?, paste_count = paste_count + ? WHERE id = ?");
  if (!st) return -1;
  sqlite3_bind_int64(st, 1, now_ms());
  sqlite3_bind_int(st, 2, pasted ? 1 : 0);
  sqlite3_bind_int64(st, 3, id);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  return rc;
}

char *db_setting_raw(struct db *db, const char *key)
{
  sqlite3_stmt *st = dbi_prep(db, "SELECT value FROM settings WHERE key = ?");
  if (!st) return NULL;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  char *v = sqlite3_step(st) == SQLITE_ROW ? xstrdup((const char *)sqlite3_column_text(st, 0)) : NULL;
  sqlite3_finalize(st);
  return v;
}

int db_setting_put(struct db *db, const char *key, const char *json)
{
  sqlite3_stmt *st = dbi_prep(db, "INSERT INTO settings(key, value) VALUES (?, ?)"
                              " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  if (!st) return -1;
  sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, json, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  return rc;
}

/* ---- retention ------------------------------------------------------- */

/* Clips the user has invested in are never removed automatically: sticky,
 * grouped, locked, bound to a hotkey or a quick-paste word, or sitting in a
 * copy buffer. */
#define EXPENDABLE \
  "sticky_order IS NULL AND locked = 0 AND group_id IS NULL AND quick_paste IS NULL" \
  " AND id NOT IN (SELECT clip_id FROM copy_buffers WHERE clip_id IS NOT NULL)" \
  " AND uuid NOT IN (SELECT arg FROM hotkeys WHERE action = 'paste_clip' AND arg IS NOT NULL)"

struct ret_ctx {
  int64_t now;
  int removed;
};

static int ret_step(struct db *db, const char *sql, int64_t a, int64_t b, int nparams)
{
  sqlite3_stmt *st = dbi_prep(db, sql);
  if (!st) return -1;
  if (nparams > 0) sqlite3_bind_int64(st, 1, a);
  if (nparams > 1) sqlite3_bind_int64(st, 2, b);
  int rc = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(db->h) : -1;
  sqlite3_finalize(st);
  return rc;
}

static int ret_tx(struct db *db, void *vctx)
{
  struct ret_ctx *c = vctx;
  const int64_t day = 86400000;
  int64_t max_age = setting_int(db, "max_age_days");
  int64_t large = setting_int(db, "large_bytes");
  int64_t large_age = setting_int(db, "large_age_days");
  int64_t max_clips = setting_int(db, "max_clips");
  int n;
  if (max_age > 0) {
    n = ret_step(db, "DELETE FROM clips WHERE " EXPENDABLE " AND last_used_at < ?", c->now - max_age * day, 0, 1);
    if (n < 0) return -1;
    c->removed += n;
  }
  if (large_age > 0) {
    n = ret_step(db, "DELETE FROM clips WHERE " EXPENDABLE " AND total_size >= ? AND last_used_at < ?",
                 large, c->now - large_age * day, 2);
    if (n < 0) return -1;
    c->removed += n;
  }
  if (max_clips > 0) {
    /* Keep the newest clips so the total stays within max_clips; protected
     * clips count toward the total but are never the ones removed. */
    n = ret_step(db,
                 "DELETE FROM clips WHERE id IN (SELECT id FROM clips WHERE " EXPENDABLE
                 " ORDER BY last_used_at DESC, id DESC LIMIT -1 OFFSET"
                 " max(0, ? - (SELECT count(*) FROM clips WHERE NOT (" EXPENDABLE "))))",
                 max_clips, 0, 1);
    if (n < 0) return -1;
    c->removed += n;
  }
  return 0;
}

int db_retention(struct db *db, int64_t now)
{
  struct ret_ctx c = { .now = now };
  if (db_tx(db, ret_tx, &c) != 0) return -1;
  if (c.removed) {
    log_info("retention removed %d clips", c.removed);
    db_blob_gc(db);
  }
  return c.removed;
}

int db_blob_gc(struct db *db)
{
  int removed = 0;
  DIR *top = opendir(db->blob_dir);
  if (!top) return 0;
  struct dirent *de;
  while ((de = readdir(top))) {
    if (strlen(de->d_name) != 2) continue;
    char *sub = xasprintf("%s/%s", db->blob_dir, de->d_name);
    DIR *d = opendir(sub);
    if (d) {
      struct dirent *fe;
      while ((fe = readdir(d))) {
        if (fe->d_name[0] == '.') continue;
        char *name = xasprintf("%s/%s", de->d_name, fe->d_name);
        /* Leftover temp files from an interrupted write are always garbage. */
        bool tmp = strstr(fe->d_name, ".tmp.") != NULL;
        if (tmp || !dbi_blob_referenced(db, name)) {
          if (blob_unlink(db->blob_dir, name) == 0) removed++;
        }
        free(name);
      }
      closedir(d);
      rmdir(sub); /* only succeeds when empty */
    }
    free(sub);
  }
  closedir(top);
  return removed;
}

cJSON *db_stats(struct db *db)
{
  cJSON *o = cJSON_CreateObject();
  sqlite3_stmt *st = dbi_prep(db, "SELECT count(*), coalesce(sum(total_size), 0) FROM clips");
  if (st && sqlite3_step(st) == SQLITE_ROW) {
    cJSON_AddNumberToObject(o, "count", (double)sqlite3_column_int64(st, 0));
    cJSON_AddNumberToObject(o, "content_bytes", (double)sqlite3_column_int64(st, 1));
  }
  sqlite3_finalize(st);
  st = dbi_prep(db, "SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()");
  if (st && sqlite3_step(st) == SQLITE_ROW) cJSON_AddNumberToObject(o, "db_bytes", (double)sqlite3_column_int64(st, 0));
  sqlite3_finalize(st);
  cJSON_AddNumberToObject(o, "schema", schema_version_latest());
  return o;
}

#include "export.h"

#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "db.h"
#include "util.h"

#define FORMAT_NAME "clipnet-export"
#define FORMAT_VERSION 1
#define MAX_IMPORT (2LL * 1024 * 1024 * 1024)

static cJSON *export_groups(struct db *db, int64_t root)
{
  cJSON *arr = cJSON_CreateArray();
  const char *sql = root
    ? "WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT g.id FROM groups g JOIN sub ON g.parent_id = sub.id)"
      " SELECT g.uuid, p.uuid, g.name FROM groups g LEFT JOIN groups p ON p.id = g.parent_id"
      " WHERE g.id IN (SELECT id FROM sub) ORDER BY g.id"
    : "SELECT g.uuid, p.uuid, g.name FROM groups g LEFT JOIN groups p ON p.id = g.parent_id ORDER BY g.id";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) return arr;
  if (root) sqlite3_bind_int64(st, 1, root);
  while (sqlite3_step(st) == SQLITE_ROW) {
    cJSON *g = cJSON_CreateObject();
    cJSON_AddStringToObject(g, "uuid", (const char *)sqlite3_column_text(st, 0));
    const char *parent = (const char *)sqlite3_column_text(st, 1);
    if (parent) cJSON_AddStringToObject(g, "parent", parent);
    else cJSON_AddNullToObject(g, "parent");
    cJSON_AddStringToObject(g, "name", (const char *)sqlite3_column_text(st, 2));
    cJSON_AddItemToArray(arr, g);
  }
  sqlite3_finalize(st);
  /* A parent outside the exported set (the root of a group export) is not
   * in the file; its child becomes top-level on import. */
  cJSON *g;
  cJSON_ArrayForEach(g, arr) {
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(g, "parent"));
    if (!parent) continue;
    bool inside = false;
    cJSON *h;
    cJSON_ArrayForEach(h, arr)
      if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(h, "uuid")), parent)) inside = true;
    if (!inside) cJSON_ReplaceItemInObject(g, "parent", cJSON_CreateNull());
  }
  return arr;
}

static const char *str_or_empty(const char *s)
{
  return s ? s : "";
}

static int write_file(const char *path, const char *text, const char **err)
{
  char *tmp = xasprintf("%s.tmp.%d", path, getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    free(tmp);
    *err = errno == ENOENT ? "that folder does not exist" : errno == EACCES ? "no permission to write there" : "cannot create the file";
    return -1;
  }
  size_t left = strlen(text);
  const char *p = text;
  while (left) {
    ssize_t w = write(fd, p, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      break;
    }
    p += w;
    left -= (size_t)w;
  }
  int rc = left == 0 && close(fd) == 0 ? rename(tmp, path) : -1;
  if (left) close(fd);
  if (rc) {
    unlink(tmp);
    *err = "writing the file failed";
  }
  free(tmp);
  return rc;
}

cJSON *export_json(struct db *db, const char *path, int64_t group_id, const char **err)
{
  if (group_id && !db_group_exists(db, group_id)) { *err = "no such group"; return NULL; }
  cJSON *doc = cJSON_CreateObject();
  cJSON_AddStringToObject(doc, "format", FORMAT_NAME);
  cJSON_AddNumberToObject(doc, "version", FORMAT_VERSION);
  cJSON_AddNumberToObject(doc, "exported_at", (double)now_ms());
  cJSON *groups = export_groups(db, group_id);
  cJSON_AddItemToObject(doc, "groups", groups);
  cJSON *clips = cJSON_AddArrayToObject(doc, "clips");

  const char *sql = group_id
    ? "WITH RECURSIVE sub(id) AS (SELECT ? UNION SELECT g.id FROM groups g JOIN sub ON g.parent_id = sub.id)"
      " SELECT c.id, c.uuid, c.created_at, c.last_used_at, c.title, g.uuid, c.sticky_order, c.locked, c.quick_paste,"
      " c.source_app FROM clips c LEFT JOIN groups g ON g.id = c.group_id"
      " WHERE c.group_id IN (SELECT id FROM sub) ORDER BY c.created_at"
    : "SELECT c.id, c.uuid, c.created_at, c.last_used_at, c.title, g.uuid, c.sticky_order, c.locked, c.quick_paste,"
      " c.source_app FROM clips c LEFT JOIN groups g ON g.id = c.group_id ORDER BY c.created_at";
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) {
    cJSON_Delete(doc);
    *err = "database error";
    return NULL;
  }
  if (group_id) sqlite3_bind_int64(st, 1, group_id);
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(st, 0);
    struct clip_payload p;
    if (db_load_payload(db, id, &p) < 0) continue;
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "uuid", (const char *)sqlite3_column_text(st, 1));
    cJSON_AddNumberToObject(c, "created_at", (double)sqlite3_column_int64(st, 2));
    cJSON_AddNumberToObject(c, "last_used_at", (double)sqlite3_column_int64(st, 3));
    const char *title = (const char *)sqlite3_column_text(st, 4);
    if (title) cJSON_AddStringToObject(c, "title", title);
    const char *g = (const char *)sqlite3_column_text(st, 5);
    if (g) cJSON_AddStringToObject(c, "group", g);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) cJSON_AddTrueToObject(c, "sticky");
    if (sqlite3_column_int(st, 7)) cJSON_AddTrueToObject(c, "locked");
    const char *qp = (const char *)sqlite3_column_text(st, 8);
    if (qp) cJSON_AddStringToObject(c, "quick_paste", qp);
    const char *app = (const char *)sqlite3_column_text(st, 9);
    if (app) cJSON_AddStringToObject(c, "source_app", app);
    cJSON *fmts = cJSON_AddArrayToObject(c, "formats");
    for (size_t i = 0; i < p.n_fmts; i++) {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "mime", p.fmts[i].mime);
      if (p.fmts[i].n_aliases) {
        cJSON *a = cJSON_AddArrayToObject(f, "aliases");
        for (size_t k = 0; k < p.fmts[i].n_aliases; k++) cJSON_AddItemToArray(a, cJSON_CreateString(p.fmts[i].aliases[k]));
      }
      char *b64 = base64_encode(p.fmts[i].data, p.fmts[i].len);
      cJSON_AddStringToObject(f, "data", b64);
      free(b64);
      cJSON_AddItemToArray(fmts, f);
    }
    clip_payload_free(&p);
    cJSON_AddItemToArray(clips, c);
    n++;
  }
  sqlite3_finalize(st);

  char *text = cJSON_PrintUnformatted(doc);
  int groups_n = cJSON_GetArraySize(groups);
  cJSON_Delete(doc);
  if (!text) { *err = "out of memory"; return NULL; }
  int rc = write_file(path, text, err);
  free(text);
  if (rc) return NULL;
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "clips", n);
  cJSON_AddNumberToObject(r, "groups", groups_n);
  cJSON_AddStringToObject(r, "path", path);
  log_info("exported %d clips and %d groups", n, groups_n);
  return r;
}

static char *slurp(const char *path, size_t *len, const char **err)
{
  FILE *f = fopen(path, "rb");
  if (!f) { *err = errno == ENOENT ? "file not found" : "cannot open the file"; return NULL; }
  struct buf b = { 0 };
  char chunk[1 << 16];
  size_t r;
  while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
    buf_append(&b, chunk, r);
    if ((long long)b.len > MAX_IMPORT) { fclose(f); buf_free(&b); *err = "the file is too large"; return NULL; }
  }
  fclose(f);
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, len);
}

/* Find or create the group for an imported uuid; parents first. */
static int64_t import_group(struct db *db, cJSON *groups, const char *uuid, int depth)
{
  if (!uuid || depth > 64) return 0;
  sqlite3_stmt *st;
  int64_t id = 0;
  if (sqlite3_prepare_v2(db_handle(db), "SELECT id FROM groups WHERE uuid = ?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  if (id) return id;
  cJSON *g, *found = NULL;
  cJSON_ArrayForEach(g, groups)
    if (!strcmp(str_or_empty(cJSON_GetStringValue(cJSON_GetObjectItem(g, "uuid"))), uuid)) found = g;
  if (!found) return 0;
  const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(found, "name"));
  int64_t parent = import_group(db, groups, cJSON_GetStringValue(cJSON_GetObjectItem(found, "parent")), depth + 1);
  const char *err;
  id = db_group_create(db, name ? name : "Imported", parent, &err);
  if (id < 0) {
    /* Same name already at that level: merge into it. */
    const char *sql = parent ? "SELECT id FROM groups WHERE parent_id = ? AND name = ?"
                             : "SELECT id FROM groups WHERE parent_id IS NULL AND name = ?";
    id = 0;
    if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) == SQLITE_OK) {
      int k = 1;
      if (parent) sqlite3_bind_int64(st, k++, parent);
      sqlite3_bind_text(st, k, name ? name : "Imported", -1, SQLITE_STATIC);
      if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
      sqlite3_finalize(st);
    }
    return id;
  }
  /* Keep the uuid so a second import of the same file finds it. */
  if (sqlite3_prepare_v2(db_handle(db), "UPDATE groups SET uuid = ? WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  return id;
}

cJSON *import_json(struct db *db, const char *path, const char **err)
{
  size_t len;
  char *raw = slurp(path, &len, err);
  if (!raw) return NULL;
  cJSON *doc = cJSON_ParseWithLength(raw, len);
  free(raw);
  if (!cJSON_IsObject(doc) || strcmp(str_or_empty(cJSON_GetStringValue(cJSON_GetObjectItem(doc, "format"))), FORMAT_NAME)) {
    cJSON_Delete(doc);
    *err = "not a CLIP//NET export file";
    return NULL;
  }
  if (cJSON_GetNumberValue(cJSON_GetObjectItem(doc, "version")) > FORMAT_VERSION) {
    cJSON_Delete(doc);
    *err = "this export was made by a newer CLIP//NET";
    return NULL;
  }
  cJSON *groups = cJSON_GetObjectItem(doc, "groups");
  cJSON *before = db_groups_list(db);
  int added = 0, dups = 0, skipped = 0, groups_before = cJSON_GetArraySize(before);
  cJSON_Delete(before);
  cJSON *c;
  cJSON_ArrayForEach(c, cJSON_GetObjectItem(doc, "clips")) {
    const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItem(c, "uuid"));
    if (uuid && db_clip_id_by_uuid(db, uuid)) { dups++; continue; }
    cJSON *fmts = cJSON_GetObjectItem(c, "formats");
    int nf = cJSON_GetArraySize(fmts);
    if (nf <= 0 || nf > 64) { skipped++; continue; }
    struct fmt_in *fi = xcalloc((size_t)nf, sizeof *fi);
    uint8_t **bufs = xcalloc((size_t)nf, sizeof *bufs);
    const char ***aliases = xcalloc((size_t)nf, sizeof *aliases);
    int k = 0;
    bool bad = false;
    cJSON *f;
    cJSON_ArrayForEach(f, fmts) {
      const char *mime = cJSON_GetStringValue(cJSON_GetObjectItem(f, "mime"));
      const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(f, "data"));
      size_t dl = 0;
      bufs[k] = data ? base64_decode(data, &dl) : NULL;
      if (!mime || !*mime || !bufs[k]) { bad = true; k++; break; }
      cJSON *al = cJSON_GetObjectItem(f, "aliases");
      int na = cJSON_GetArraySize(al);
      if (na > 0 && na <= 16) {
        aliases[k] = xcalloc((size_t)na, sizeof **aliases);
        int j = 0;
        cJSON *a;
        cJSON_ArrayForEach(a, al) if (cJSON_IsString(a)) aliases[k][j++] = a->valuestring;
        fi[k].aliases = aliases[k];
        fi[k].n_aliases = (size_t)j;
      }
      fi[k].mime = mime;
      fi[k].data = bufs[k];
      fi[k].len = dl;
      k++;
    }
    if (!bad) {
      struct clip_in in = {
        .fmts = fi,
        .n_fmts = (size_t)k,
        .uuid = uuid,
        .created_at = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(c, "created_at")),
        .last_used_at = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(c, "last_used_at")),
        .source_app = cJSON_GetStringValue(cJSON_GetObjectItem(c, "source_app")),
        .title = cJSON_GetStringValue(cJSON_GetObjectItem(c, "title")),
        .group_id = import_group(db, groups, cJSON_GetStringValue(cJSON_GetObjectItem(c, "group")), 0),
      };
      int64_t id;
      enum add_result r = db_add_clip(db, &in, &id);
      if (r == ADD_NEW) {
        added++;
        const char *e;
        if (cJSON_IsTrue(cJSON_GetObjectItem(c, "sticky"))) db_clip_set_sticky(db, id, STICKY_BOTTOM, &e);
        if (cJSON_IsTrue(cJSON_GetObjectItem(c, "locked"))) db_clip_set_locked(db, id, true, &e);
        const char *qp = cJSON_GetStringValue(cJSON_GetObjectItem(c, "quick_paste"));
        if (qp) db_clip_set_quick_paste(db, id, qp, &e); /* a clash just drops the word */
      } else if (r == ADD_DUP) {
        dups++;
      } else {
        skipped++;
      }
    } else {
      skipped++;
    }
    for (int i = 0; i < nf; i++) {
      free(bufs[i]);
      free(aliases[i]);
    }
    free(bufs);
    free(aliases);
    free(fi);
  }
  cJSON_Delete(doc);
  cJSON *after = db_groups_list(db);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "added", added);
  cJSON_AddNumberToObject(r, "duplicates", dups);
  cJSON_AddNumberToObject(r, "skipped", skipped);
  cJSON_AddNumberToObject(r, "groups", cJSON_GetArraySize(after) - groups_before);
  cJSON_Delete(after);
  log_info("imported %d clips (%d duplicates, %d skipped)", added, dups, skipped);
  return r;
}

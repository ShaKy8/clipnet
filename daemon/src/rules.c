#include "rules.h"

#include <fnmatch.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "cJSON.h"
#include "db.h"
#include "hypr.h"
#include "mime.h"
#include "util.h"

static bool glob(const char *pattern, const char *s)
{
  return pattern && s && fnmatch(pattern, s, FNM_CASEFOLD) == 0;
}

/* Rows of one action, enabled, in order. The caller steps and finalizes. */
static sqlite3_stmt *select_action(struct db *db, const char *action)
{
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db),
                         "SELECT match_app, match_mime, arg FROM rules WHERE enabled = 1 AND action = ? ORDER BY ord, id",
                         -1, &st, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_text(st, 1, action, -1, SQLITE_STATIC);
  return st;
}

bool rules_excluded(struct db *db, const char *app)
{
  if (!app || !*app) return false;
  sqlite3_stmt *st = select_action(db, "exclude");
  if (!st) return false;
  bool hit = false;
  while (!hit && sqlite3_step(st) == SQLITE_ROW) hit = glob((const char *)sqlite3_column_text(st, 0), app);
  sqlite3_finalize(st);
  return hit;
}

void rules_filter_plan(struct db *db, const char *app, struct mime_plan *plan)
{
  sqlite3_stmt *st = select_action(db, "skip_mime");
  if (!st) return;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *app_pat = (const char *)sqlite3_column_text(st, 0);
    const char *mime_pat = (const char *)sqlite3_column_text(st, 1);
    if (!mime_pat) continue;
    if (app_pat && !glob(app_pat, app)) continue;
    size_t w = 0;
    for (size_t r = 0; r < plan->n_reads; r++) {
      /* Match what the user sees in the formats list (the canonical name)
       * and what the source called it. */
      if (glob(mime_pat, plan->reads[r].canonical) || glob(mime_pat, plan->reads[r].offered)) continue;
      plan->reads[w++] = plan->reads[r];
    }
    plan->n_reads = w;
  }
  sqlite3_finalize(st);
}

int64_t rules_route_group(struct db *db, const char *app)
{
  if (!app || !*app) return 0;
  sqlite3_stmt *st = select_action(db, "to_group");
  if (!st) return 0;
  int64_t group = 0;
  while (!group && sqlite3_step(st) == SQLITE_ROW) {
    if (!glob((const char *)sqlite3_column_text(st, 0), app)) continue;
    const char *arg = (const char *)sqlite3_column_text(st, 2);
    cJSON *a = arg ? cJSON_Parse(arg) : NULL;
    const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItem(a, "group"));
    sqlite3_stmt *g;
    if (uuid && sqlite3_prepare_v2(db_handle(db), "SELECT id FROM groups WHERE uuid = ?", -1, &g, NULL) == SQLITE_OK) {
      sqlite3_bind_text(g, 1, uuid, -1, SQLITE_STATIC);
      if (sqlite3_step(g) == SQLITE_ROW) group = sqlite3_column_int64(g, 0);
      sqlite3_finalize(g);
    }
    cJSON_Delete(a);
  }
  sqlite3_finalize(st);
  return group;
}

size_t rules_paste_keys(struct db *db, struct paste_key **out)
{
  *out = NULL;
  sqlite3_stmt *st = select_action(db, "paste_keys");
  if (!st) return 0;
  size_t n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *pat = (const char *)sqlite3_column_text(st, 0);
    const char *arg = (const char *)sqlite3_column_text(st, 2);
    cJSON *a = arg ? cJSON_Parse(arg) : NULL;
    const char *keys = cJSON_GetStringValue(cJSON_GetObjectItem(a, "keys"));
    struct accel acc;
    const char *err;
    if (pat && keys && accel_parse_send(keys, &acc, &err)) {
      char mods[32];
      accel_hypr_mods(&acc, mods);
      *out = xrealloc(*out, (n + 1) * sizeof **out);
      (*out)[n++] = (struct paste_key){ xstrdup(pat), xstrdup(mods), xstrdup(accel_keysym(&acc)) };
    }
    cJSON_Delete(a);
  }
  sqlite3_finalize(st);
  return n;
}

void rules_paste_keys_free(struct paste_key *keys, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    free((char *)keys[i].pattern);
    free((char *)keys[i].mods);
    free((char *)keys[i].key);
  }
  free(keys);
}

/* ---- editing ------------------------------------------------------------ */

cJSON *rules_list(struct db *db)
{
  cJSON *arr = cJSON_CreateArray();
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db),
                         "SELECT id, enabled, action, match_app, match_mime, arg FROM rules ORDER BY ord, id", -1, &st,
                         NULL) != SQLITE_OK)
    return arr;
  while (sqlite3_step(st) == SQLITE_ROW) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)sqlite3_column_int64(st, 0));
    cJSON_AddBoolToObject(r, "enabled", sqlite3_column_int(st, 1));
    cJSON_AddStringToObject(r, "action", (const char *)sqlite3_column_text(st, 2));
    for (int c = 3; c <= 4; c++) {
      const char *v = (const char *)sqlite3_column_text(st, c);
      const char *k = c == 3 ? "match_app" : "match_mime";
      if (v) cJSON_AddStringToObject(r, k, v);
      else cJSON_AddNullToObject(r, k);
    }
    const char *arg = (const char *)sqlite3_column_text(st, 5);
    cJSON *a = arg ? cJSON_Parse(arg) : NULL;
    cJSON_AddItemToObject(r, "arg", a ? a : cJSON_CreateNull());
    cJSON_AddItemToArray(arr, r);
  }
  sqlite3_finalize(st);
  return arr;
}

static const char *nonempty(const cJSON *spec, const char *key)
{
  const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(spec, key));
  return s && *s ? s : NULL;
}

int64_t rules_set(struct db *db, int64_t id, const cJSON *spec, const char **err)
{
  const char *action = nonempty(spec, "action");
  const char *app = nonempty(spec, "match_app");
  const char *mime = nonempty(spec, "match_mime");
  const cJSON *arg = cJSON_GetObjectItemCaseSensitive(spec, "arg");
  const cJSON *en = cJSON_GetObjectItemCaseSensitive(spec, "enabled");
  bool enabled = !en || cJSON_IsTrue(en);
  cJSON *stored = NULL;

  if (!action) { *err = "a rule needs an action"; return -1; }
  if (!strcmp(action, "exclude")) {
    if (!app) { *err = "say which app (e.g. *keepassxc*)"; return -1; }
    mime = NULL;
  } else if (!strcmp(action, "skip_mime")) {
    if (!mime) { *err = "say which format (e.g. image/* or text/html)"; return -1; }
  } else if (!strcmp(action, "paste_keys")) {
    if (!app) { *err = "say which app"; return -1; }
    const char *keys = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arg, "keys"));
    struct accel a;
    if (!keys || !accel_parse_send(keys, &a, err)) { if (!keys) *err = "say which keys paste (e.g. Ctrl+Shift+V)"; return -1; }
    char canon[64];
    accel_format(&a, canon);
    stored = cJSON_CreateObject();
    cJSON_AddStringToObject(stored, "keys", canon);
    mime = NULL;
  } else if (!strcmp(action, "to_group")) {
    if (!app) { *err = "say which app"; return -1; }
    const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(arg, "group"));
    sqlite3_stmt *g;
    bool found = false;
    if (uuid && sqlite3_prepare_v2(db_handle(db), "SELECT 1 FROM groups WHERE uuid = ?", -1, &g, NULL) == SQLITE_OK) {
      sqlite3_bind_text(g, 1, uuid, -1, SQLITE_STATIC);
      found = sqlite3_step(g) == SQLITE_ROW;
      sqlite3_finalize(g);
    }
    if (!found) { *err = "choose a group"; return -1; }
    stored = cJSON_CreateObject();
    cJSON_AddStringToObject(stored, "group", uuid);
    mime = NULL;
  } else {
    *err = "unknown rule action";
    return -1;
  }

  char *arg_text = stored ? cJSON_PrintUnformatted(stored) : NULL;
  cJSON_Delete(stored);
  sqlite3_stmt *st;
  const char *sql = id ? "UPDATE rules SET enabled = ?, action = ?, match_app = ?, match_mime = ?, arg = ? WHERE id = ?"
                       : "INSERT INTO rules(enabled, action, match_app, match_mime, arg, ord)"
                         " VALUES (?, ?, ?, ?, ?, (SELECT coalesce(max(ord), 0) + 1 FROM rules))";
  if (sqlite3_prepare_v2(db_handle(db), sql, -1, &st, NULL) != SQLITE_OK) {
    free(arg_text);
    *err = "database error";
    return -1;
  }
  sqlite3_bind_int(st, 1, enabled);
  sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, app, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, mime, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 5, arg_text, -1, SQLITE_STATIC);
  if (id) sqlite3_bind_int64(st, 6, id);
  bool ok = sqlite3_step(st) == SQLITE_DONE && (!id || sqlite3_changes(db_handle(db)) == 1);
  sqlite3_finalize(st);
  free(arg_text);
  if (!ok) { *err = id ? "no such rule" : "database error"; return -1; }
  return id ? id : sqlite3_last_insert_rowid(db_handle(db));
}

int rules_delete(struct db *db, int64_t id, const char **err)
{
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db), "DELETE FROM rules WHERE id = ?", -1, &st, NULL) != SQLITE_OK) {
    *err = "database error";
    return -1;
  }
  sqlite3_bind_int64(st, 1, id);
  bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_handle(db)) == 1;
  sqlite3_finalize(st);
  if (!ok) { *err = "no such rule"; return -1; }
  return 0;
}

/* The named rules go first, in the order given; every other rule keeps its
 * relative order after them. Positions are rewritten as 1..N. */
int rules_reorder(struct db *db, const int64_t *ids, size_t n, const char **err)
{
  sqlite3 *h = db_handle(db);
  sqlite3_stmt *st;
  int64_t *order = NULL;
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    bool dup = false;
    for (size_t k = 0; k < count; k++) dup |= order[k] == ids[i];
    if (dup) continue;
    order = xrealloc(order, (count + 1) * sizeof *order);
    order[count++] = ids[i];
  }
  if (sqlite3_prepare_v2(h, "SELECT id FROM rules ORDER BY ord, id", -1, &st, NULL) != SQLITE_OK) {
    free(order);
    *err = "database error";
    return -1;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(st, 0);
    bool named = false;
    for (size_t i = 0; i < n; i++) named |= ids[i] == id;
    if (named) continue;
    order = xrealloc(order, (count + 1) * sizeof *order);
    order[count++] = id;
  }
  sqlite3_finalize(st);

  if (sqlite3_prepare_v2(h, "UPDATE rules SET ord = ? WHERE id = ?", -1, &st, NULL) != SQLITE_OK) {
    free(order);
    *err = "database error";
    return -1;
  }
  sqlite3_exec(h, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  for (size_t i = 0; i < count; i++) {
    sqlite3_reset(st);
    sqlite3_bind_double(st, 1, (double)(i + 1));
    sqlite3_bind_int64(st, 2, order[i]);
    if (sqlite3_step(st) != SQLITE_DONE) {
      sqlite3_finalize(st);
      sqlite3_exec(h, "ROLLBACK", NULL, NULL, NULL);
      free(order);
      *err = "database error";
      return -1;
    }
  }
  sqlite3_finalize(st);
  sqlite3_exec(h, "COMMIT", NULL, NULL, NULL);
  free(order);
  return 0;
}

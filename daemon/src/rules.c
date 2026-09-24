#include "rules.h"

#include <fnmatch.h>
#include <sqlite3.h>

#include "cJSON.h"
#include "db.h"
#include "hypr.h"
#include "util.h"

#include <stdlib.h>

bool rules_excluded(struct db *db, const char *app)
{
  if (!app || !*app) return false;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db),
                         "SELECT match_app FROM rules WHERE enabled = 1 AND action = 'exclude'"
                         " AND match_app IS NOT NULL ORDER BY ord",
                         -1, &st, NULL) != SQLITE_OK)
    return false;
  bool hit = false;
  while (!hit && sqlite3_step(st) == SQLITE_ROW) {
    const char *pat = (const char *)sqlite3_column_text(st, 0);
    hit = pat && fnmatch(pat, app, FNM_CASEFOLD) == 0;
  }
  sqlite3_finalize(st);
  return hit;
}

size_t rules_paste_keys(struct db *db, struct paste_key **out)
{
  *out = NULL;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(db),
                         "SELECT match_app, arg FROM rules WHERE enabled = 1 AND action = 'paste_keys'"
                         " AND match_app IS NOT NULL ORDER BY ord",
                         -1, &st, NULL) != SQLITE_OK)
    return 0;
  size_t n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *pat = (const char *)sqlite3_column_text(st, 0);
    const char *arg = (const char *)sqlite3_column_text(st, 1);
    cJSON *a = arg ? cJSON_Parse(arg) : NULL;
    const char *mods = cJSON_GetStringValue(cJSON_GetObjectItem(a, "mods"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(a, "key"));
    if (pat && mods && key && *key) {
      *out = xrealloc(*out, (n + 1) * sizeof **out);
      (*out)[n++] = (struct paste_key){ xstrdup(pat), xstrdup(mods), xstrdup(key) };
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

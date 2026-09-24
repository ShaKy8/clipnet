#include "settings.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "db.h"
#include "util.h"

#define MiB (1024 * 1024)

/* Defaults marked "Ditto" follow Ditto's behaviour; the rest are ours. */
static const struct setting_def defs[] = {
  /* Retention. Unlimited history; only big payloads age out. */
  { "max_clips", SET_INT, "0", 0, 10000000, NULL },
  { "max_age_days", SET_INT, "0", 0, 36500, NULL },
  { "large_bytes", SET_INT, "26214400", 1024, 4096LL * MiB, NULL },
  { "large_age_days", SET_INT, "30", 0, 36500, NULL },

  /* Capture. */
  { "paused", SET_BOOL, "false", 0, 0, NULL },
  { "paused_until", SET_INT, "0", 0, INT64_MAX, NULL },
  { "capture_primary", SET_BOOL, "false", 0, 0, NULL },
  { "keep_alive", SET_BOOL, "true", 0, 0, NULL },
  { "store_source_title", SET_BOOL, "false", 0, 0, NULL },
  { "text_cap_bytes", SET_INT, "16777216", 1024, 256LL * MiB, NULL },
  { "image_cap_bytes", SET_INT, "67108864", 1024, 1024LL * MiB, NULL },
  { "other_cap_bytes", SET_INT, "8388608", 1024, 256LL * MiB, NULL },

  /* Paste. */
  { "move_to_top_on_paste", SET_BOOL, "true", 0, 0, NULL },
  { "paste_delay_ms", SET_INT, "30", 0, 2000, NULL },
  { "multi_separator", SET_STRING, "\"\\n\"", 0, 0, NULL },
  { "date_format", SET_STRING, "\"%Y-%m-%d %H:%M\"", 0, 0, NULL },

  /* Popup. */
  { "popup_position", SET_STRING, "\"cursor\"", 0, 0, "|cursor|center|last|" },
  { "popup_width", SET_INT, "520", 280, 2000, NULL },
  { "popup_rows", SET_INT, "14", 4, 60, NULL },
  { "show_grouped_in_history", SET_BOOL, "true", 0, 0, NULL },
  { "esc_clears_search", SET_BOOL, "true", 0, 0, NULL },
  { "show_thumbnails", SET_BOOL, "true", 0, 0, NULL },
  { "preview_on_hover", SET_BOOL, "true", 0, 0, NULL },
  { "scrim_alpha", SET_INT, "0", 0, 100, NULL },
};

const struct setting_def *setting_find(const char *key)
{
  for (size_t i = 0; i < ARRAY_LEN(defs); i++)
    if (!strcmp(defs[i].key, key)) return &defs[i];
  return NULL;
}

const struct setting_def *settings_all(size_t *n)
{
  *n = ARRAY_LEN(defs);
  return defs;
}

/* Stored value, or the default. Always returns a parsed JSON value. */
static cJSON *value_of(struct db *db, const struct setting_def *d)
{
  char *raw = db_setting_raw(db, d->key);
  cJSON *v = raw ? cJSON_Parse(raw) : NULL;
  free(raw);
  if (!v) v = cJSON_Parse(d->def);
  return v;
}

bool setting_bool(struct db *db, const char *key)
{
  const struct setting_def *d = setting_find(key);
  if (!d) { log_err("unknown setting %s", key); return false; }
  cJSON *v = value_of(db, d);
  bool r = cJSON_IsTrue(v);
  cJSON_Delete(v);
  return r;
}

int64_t setting_int(struct db *db, const char *key)
{
  const struct setting_def *d = setting_find(key);
  if (!d) { log_err("unknown setting %s", key); return 0; }
  cJSON *v = value_of(db, d);
  int64_t r = cJSON_IsNumber(v) ? (int64_t)cJSON_GetNumberValue(v) : 0;
  cJSON_Delete(v);
  return r;
}

char *setting_string(struct db *db, const char *key)
{
  const struct setting_def *d = setting_find(key);
  if (!d) { log_err("unknown setting %s", key); return xstrdup(""); }
  cJSON *v = value_of(db, d);
  char *r = xstrdup(cJSON_IsString(v) ? cJSON_GetStringValue(v) : "");
  cJSON_Delete(v);
  return r;
}

int setting_set(struct db *db, const char *key, const cJSON *value, const char **err)
{
  const struct setting_def *d = setting_find(key);
  if (!d) { *err = "unknown setting"; return -1; }
  switch (d->type) {
  case SET_BOOL:
    if (!cJSON_IsBool(value)) { *err = "expected true or false"; return -1; }
    break;
  case SET_INT: {
    if (!cJSON_IsNumber(value)) { *err = "expected a number"; return -1; }
    double x = cJSON_GetNumberValue(value);
    if (x != (double)(int64_t)x) { *err = "expected a whole number"; return -1; }
    if ((int64_t)x < d->min || (int64_t)x > d->max) { *err = "out of range"; return -1; }
    break;
  }
  case SET_STRING:
    if (!cJSON_IsString(value)) { *err = "expected a string"; return -1; }
    if (d->choices) {
      char *needle = xasprintf("|%s|", cJSON_GetStringValue(value));
      bool ok = strstr(d->choices, needle) != NULL;
      free(needle);
      if (!ok) { *err = "not one of the allowed values"; return -1; }
    }
    break;
  }
  char *json = cJSON_PrintUnformatted(value);
  int rc = db_setting_put(db, key, json);
  free(json);
  if (rc) *err = "database error";
  return rc;
}

cJSON *settings_dump(struct db *db)
{
  cJSON *o = cJSON_CreateObject();
  for (size_t i = 0; i < ARRAY_LEN(defs); i++)
    cJSON_AddItemToObject(o, defs[i].key, value_of(db, &defs[i]));
  return o;
}

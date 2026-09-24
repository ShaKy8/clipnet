#include "import.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "util.h"

#define MAX_IMPORT_FILE (512 * 1024 * 1024)

static char *slurp(const char *path, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  struct buf b = { 0 };
  char chunk[65536];
  size_t r;
  while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
    buf_append(&b, chunk, r);
    if (b.len > MAX_IMPORT_FILE) break;
  }
  bool bad = ferror(f) || b.len > MAX_IMPORT_FILE;
  fclose(f);
  if (bad) { buf_free(&b); return NULL; }
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, len);
}

cJSON *import_omarchy(struct db *db, const char *path, const char **err)
{
  size_t len;
  char *raw = slurp(path, &len);
  if (!raw) { *err = errno == ENOENT ? "no Omarchy clipboard history found" : "could not read the history file"; return NULL; }
  cJSON *arr = cJSON_ParseWithLength(raw, len);
  free(raw);
  if (!cJSON_IsArray(arr)) {
    cJSON_Delete(arr);
    *err = "the history file is not a JSON array";
    return NULL;
  }

  int n = cJSON_GetArraySize(arr), added = 0, dups = 0, skipped = 0;
  /* Omarchy keeps no usable timestamps ("Monday 14:02"), only order, newest
   * first. Space the entries a second apart ending now, and insert oldest
   * first so ids follow the same order. */
  int64_t now = now_ms();
  for (int i = n - 1; i >= 0; i--) {
    cJSON *e = cJSON_GetArrayItem(arr, i);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(e, "type"));
    int64_t at = now - (int64_t)i * 1000;
    struct fmt_in f = { 0 };
    char *data = NULL;
    if (type && !strcmp(type, "text")) {
      const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(e, "text"));
      if (!text || !*text) { skipped++; continue; }
      f = (struct fmt_in){ MIME_TEXT, (const uint8_t *)text, strlen(text), mime_text_default_aliases,
                           mime_text_default_alias_count };
    } else if (type && !strcmp(type, "image")) {
      const char *ipath = cJSON_GetStringValue(cJSON_GetObjectItem(e, "path"));
      const char *mime = cJSON_GetStringValue(cJSON_GetObjectItem(e, "mime"));
      size_t dlen = 0;
      data = ipath ? slurp(ipath, &dlen) : NULL;
      if (!data || !dlen) { free(data); skipped++; continue; }
      f = (struct fmt_in){ mime && mime_is_image(mime) ? mime : "image/png", (const uint8_t *)data, dlen, NULL, 0 };
    } else {
      skipped++;
      continue;
    }
    struct clip_in in = { .fmts = &f, .n_fmts = 1, .created_at = at };
    enum add_result r = db_add_clip(db, &in, NULL);
    if (r == ADD_NEW) added++;
    else if (r == ADD_DUP) dups++;
    else skipped++;
    free(data);
  }
  cJSON_Delete(arr);
  cJSON *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(res, "added", added);
  cJSON_AddNumberToObject(res, "duplicates", dups);
  cJSON_AddNumberToObject(res, "skipped", skipped);
  log_info("imported Omarchy history: %d added, %d duplicates, %d skipped", added, dups, skipped);
  return res;
}

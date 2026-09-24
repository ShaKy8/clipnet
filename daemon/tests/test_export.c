#include "harness.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "db.h"
#include "export.h"
#include "mime.h"
#include "util.h"

static struct db *open_db(const char *name)
{
  char *dir = xasprintf("%s/%s", test_tmpdir(), name);
  struct db *db = db_open(dir);
  free(dir);
  return db;
}

static int num(cJSON *o, const char *k)
{
  return (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, k));
}

void run_tests(void)
{
  struct db *a = open_db("export-a"), *b = open_db("export-b");
  const char *err = NULL;

  int64_t work = db_group_create(a, "Work", 0, &err);
  int64_t sub = db_group_create(a, "Snippets", work, &err);
  uint8_t bin[300];
  for (int i = 0; i < 300; i++) bin[i] = (uint8_t)(i * 7);
  const char *aliases[] = { "UTF8_STRING", "text/plain" };
  struct fmt_in rich[] = {
    { MIME_TEXT, (const uint8_t *)"Hello ✓", strlen("Hello ✓"), aliases, 2 },
    { "text/html", (const uint8_t *)"<b>Hello ✓</b>", strlen("<b>Hello ✓</b>"), NULL, 0 },
    { "application/x-binary", bin, sizeof bin, NULL, 0 },
  };
  struct clip_in rin = { .fmts = rich, .n_fmts = 3, .created_at = 1000, .source_app = "firefox" };
  int64_t r_id;
  db_add_clip(a, &rin, &r_id);
  db_clip_move(a, r_id, sub, &err);
  db_clip_set_title(a, r_id, "Greeting", &err);
  db_clip_set_sticky(a, r_id, STICKY_TOP, &err);
  db_clip_set_locked(a, r_id, true, &err);
  db_clip_set_quick_paste(a, r_id, "hi", &err);
  struct fmt_in plain = { MIME_TEXT, (const uint8_t *)"loose", 5, NULL, 0 };
  struct clip_in pin = { .fmts = &plain, .n_fmts = 1, .created_at = 2000 };
  db_add_clip(a, &pin, NULL);

  char *path = xasprintf("%s/all.json", test_tmpdir());
  cJSON *res = export_json(a, path, 0, &err);
  CHECK(res != NULL);
  CHECK_INT(num(res, "clips"), 2);
  CHECK_INT(num(res, "groups"), 2);
  cJSON_Delete(res);
  struct stat st;
  CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

  res = import_json(b, path, &err);
  CHECK(res != NULL);
  CHECK_INT(num(res, "added"), 2);
  CHECK_INT(num(res, "groups"), 2);
  cJSON_Delete(res);

  /* Everything came across: formats byte for byte, and the organisation. */
  char uuid[37];
  db_clip_uuid(a, r_id, uuid);
  int64_t copy = db_clip_id_by_uuid(b, uuid);
  CHECK(copy > 0);
  struct clip_payload p;
  CHECK_INT(db_load_payload(b, copy, &p), 0);
  CHECK_INT(p.n_fmts, 3);
  CHECK_STR(p.fmts[0].mime, MIME_TEXT);
  CHECK_INT(p.fmts[0].n_aliases, 2);
  CHECK(p.fmts[2].len == sizeof bin && !memcmp(p.fmts[2].data, bin, sizeof bin));
  clip_payload_free(&p);
  cJSON *row = db_row(b, copy);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(row, "title")), "Greeting");
  CHECK(cJSON_IsTrue(cJSON_GetObjectItem(row, "sticky")));
  CHECK(cJSON_IsTrue(cJSON_GetObjectItem(row, "locked")));
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(row, "quick_paste")), "hi");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(row, "source_app")), "firefox");
  CHECK_INT(num(row, "created_at"), 1000);
  int64_t gid = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "group_id"));
  cJSON_Delete(row);
  cJSON *groups = db_groups_list(b);
  bool nested = false;
  cJSON *g;
  cJSON_ArrayForEach(g, groups) {
    if ((int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(g, "id")) == gid)
      nested = !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g, "name")), "Snippets") &&
               cJSON_IsNumber(cJSON_GetObjectItem(g, "parent_id"));
  }
  CHECK(nested);
  cJSON_Delete(groups);

  /* A second import changes nothing. */
  res = import_json(b, path, &err);
  CHECK_INT(num(res, "added"), 0);
  CHECK_INT(num(res, "duplicates"), 2);
  CHECK_INT(num(res, "groups"), 0);
  cJSON_Delete(res);

  /* Exporting one group takes its subgroups, not the loose clip, and its
   * root becomes top-level in the file. */
  char *gpath = xasprintf("%s/group.json", test_tmpdir());
  res = export_json(a, gpath, work, &err);
  CHECK_INT(num(res, "clips"), 1);
  CHECK_INT(num(res, "groups"), 2);
  cJSON_Delete(res);

  CHECK(import_json(b, "/nonexistent/x.json", &err) == NULL);
  CHECK_STR(err, "file not found");
  char *junk = xasprintf("%s/junk.json", test_tmpdir());
  FILE *f = fopen(junk, "w");
  fputs("{\"format\":\"something-else\"}", f);
  fclose(f);
  CHECK(import_json(b, junk, &err) == NULL);
  CHECK(export_json(a, "/nonexistent-dir/x.json", 0, &err) == NULL);

  free(path);
  free(gpath);
  free(junk);
  db_close(a);
  db_close(b);
}

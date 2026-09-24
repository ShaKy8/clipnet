#include "harness.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "settings.h"
#include "util.h"

static struct db *fresh(const char *name)
{
  char *dir = xasprintf("%s/%s", test_tmpdir(), name);
  struct db *db = db_open(dir);
  free(dir);
  return db;
}

static int64_t add_text(struct db *db, const char *text, int64_t at, enum add_result *res)
{
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)text, strlen(text), NULL, 0 };
  struct clip_in in = { .fmts = &f, .n_fmts = 1, .created_at = at, .source_app = "test" };
  int64_t id = 0;
  enum add_result r = db_add_clip(db, &in, &id);
  if (res) *res = r;
  return id;
}

static cJSON *list(struct db *db, const char *q)
{
  struct list_query lq = { .query = q, .limit = 100 };
  return db_list(db, &lq);
}

static int list_total(struct db *db, const char *q)
{
  cJSON *r = list(db, q);
  int n = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(r, "total"));
  cJSON_Delete(r);
  return n;
}

static const char *row_str(cJSON *r, int i, const char *k)
{
  return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "rows"), i), k));
}

static void test_open_and_schema(void)
{
  struct db *db = fresh("schema");
  CHECK(db != NULL);
  if (!db) return;
  char *path = xasprintf("%s/clipnet.db", db_dir(db));
  struct stat st;
  CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  free(path);
  cJSON *s = db_stats(db);
  CHECK_INT(cJSON_GetNumberValue(cJSON_GetObjectItem(s, "count")), 0);
  cJSON_Delete(s);
  db_close(db);
  /* Reopening an existing database must not re-run the migration. */
  db = fresh("schema");
  CHECK(db != NULL);
  db_close(db);
}

static void test_add_dedupe_order(void)
{
  struct db *db = fresh("add");
  enum add_result r;
  int64_t a = add_text(db, "alpha one", 1000, &r);
  CHECK_INT(r, ADD_NEW);
  int64_t b = add_text(db, "bravo two", 2000, &r);
  CHECK_INT(r, ADD_NEW);
  CHECK(a && b && a != b);

  cJSON *l = list(db, NULL);
  CHECK_STR(row_str(l, 0, "preview"), "bravo two");
  CHECK_STR(row_str(l, 1, "preview"), "alpha one");
  CHECK_STR(row_str(l, 0, "kind"), "text");
  cJSON_Delete(l);

  /* Copying alpha again moves it to the top, no new row. */
  int64_t a2 = add_text(db, "alpha one", 3000, &r);
  CHECK_INT(r, ADD_DUP);
  CHECK_INT(a2, a);
  l = list(db, NULL);
  CHECK_STR(row_str(l, 0, "preview"), "alpha one");
  CHECK_INT(cJSON_GetNumberValue(cJSON_GetObjectItem(l, "total")), 2);
  cJSON_Delete(l);
  CHECK_INT(db_id_at_position(db, 0), a);
  CHECK_INT(db_id_at_position(db, 1), b);
  CHECK_INT(db_id_at_position(db, 2), 0);
  db_close(db);
}

static void test_search(void)
{
  struct db *db = fresh("search");
  add_text(db, "SELECT * FROM users WHERE id = 1", 1000, NULL);
  add_text(db, "curl -H \"Authorization: Bearer x\"", 2000, NULL);
  add_text(db, "héllo wörld", 3000, NULL);
  add_text(db, "a b c", 4000, NULL);

  CHECK_INT(list_total(db, "users"), 1);
  CHECK_INT(list_total(db, "USERS"), 1);             /* case-insensitive */
  CHECK_INT(list_total(db, "from users"), 1);        /* terms are ANDed */
  CHECK_INT(list_total(db, "from curl"), 0);
  CHECK_INT(list_total(db, "ser"), 1);               /* substring, not prefix */
  CHECK_INT(list_total(db, "wö"), 1);                /* short: LIKE path */
  CHECK_INT(list_total(db, "b"), 2);                 /* "Bearer" and "a b c" */
  CHECK_INT(list_total(db, "\"Authorization:"), 1);  /* quotes are escaped */
  CHECK_INT(list_total(db, "100%"), 0);              /* % is literal */
  CHECK_INT(list_total(db, "_"), 0);                 /* _ is literal */
  CHECK_INT(list_total(db, ""), 4);
  db_close(db);
}

static void test_payload_roundtrip(void)
{
  struct db *db = fresh("payload");
  const char *aliases[] = { "text/plain;charset=utf-8", "UTF8_STRING" };
  size_t big_len = 200 * 1024;
  uint8_t *big = xmalloc(big_len);
  for (size_t i = 0; i < big_len; i++) big[i] = (uint8_t)(i * 31);
  struct fmt_in f[] = {
    { MIME_TEXT, (const uint8_t *)"bold", 4, aliases, 2 },
    { "text/html", (const uint8_t *)"<b>bold</b>", 11, NULL, 0 },
    { "application/x-big", big, big_len, NULL, 0 },
    { "application/x-empty", (const uint8_t *)"", 0, NULL, 0 },
  };
  struct clip_in in = { .fmts = f, .n_fmts = 4 };
  int64_t id;
  CHECK_INT(db_add_clip(db, &in, &id), ADD_NEW);

  struct clip_payload p;
  CHECK_INT(db_load_payload(db, id, &p), 0);
  CHECK_INT(p.n_fmts, 4);
  CHECK_STR(p.fmts[0].mime, MIME_TEXT);
  CHECK_INT(p.fmts[0].n_aliases, 2);
  CHECK_STR(p.fmts[0].aliases[1], "UTF8_STRING");
  CHECK(p.fmts[1].len == 11 && !memcmp(p.fmts[1].data, "<b>bold</b>", 11));
  CHECK(p.fmts[2].map_len == big_len); /* large: blob file, mapped */
  CHECK(p.fmts[2].len == big_len && !memcmp(p.fmts[2].data, big, big_len));
  CHECK_INT(p.fmts[3].len, 0);
  clip_payload_free(&p);

  cJSON *g = db_get(db, id);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(g, "text")), "bold");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(g, "kind")), "rich");
  CHECK_INT(cJSON_GetArraySize(cJSON_GetObjectItem(g, "formats")), 4);
  cJSON_Delete(g);

  /* Same formats in another order are the same clip. */
  struct fmt_in rev[] = { f[3], f[2], f[1], f[0] };
  struct clip_in in2 = { .fmts = rev, .n_fmts = 4 };
  int64_t id2;
  CHECK_INT(db_add_clip(db, &in2, &id2), ADD_DUP);
  CHECK_INT(id2, id);

  /* Deleting removes the blob file with it. */
  cJSON *d = db_get(db, id);
  const char *blob_path = NULL;
  cJSON *it;
  cJSON_ArrayForEach(it, cJSON_GetObjectItem(d, "formats")) {
    cJSON *pth = cJSON_GetObjectItem(it, "path");
    if (pth) blob_path = cJSON_GetStringValue(pth);
  }
  CHECK(blob_path && access(blob_path, F_OK) == 0);
  CHECK_INT(db_delete(db, id), 0);
  CHECK(blob_path && access(blob_path, F_OK) != 0);
  cJSON_Delete(d);
  CHECK(!db_exists(db, id));
  CHECK_INT(db_delete(db, id), 1); /* already gone */
  free(big);
  db_close(db);
}

static void test_image_and_html_only(void)
{
  struct db *db = fresh("image");
  static const uint8_t png[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
    0, 0, 0x07, 0x80, 0, 0, 0x04, 0x38, 8, 6, 0, 0, 0,
  };
  struct fmt_in f = { "image/png", png, sizeof png, NULL, 0 };
  struct clip_in in = { .fmts = &f, .n_fmts = 1 };
  int64_t id;
  CHECK_INT(db_add_clip(db, &in, &id), ADD_NEW);
  cJSON *r = db_row(db, id);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "kind")), "image");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "preview")), "[PNG 1920×1080 · 29 B]");
  const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(r, "image"));
  CHECK(img && access(img, R_OK) == 0);
  cJSON_Delete(r);
  /* The list path must report the same image (its SQL is built differently). */
  cJSON *l = list(db, NULL);
  const char *limg = row_str(l, 0, "image");
  CHECK(limg && access(limg, R_OK) == 0);
  CHECK_INT(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(l, "rows"), 0), "mimes")), 1);
  cJSON_Delete(l);

  struct fmt_in h = { "text/html", (const uint8_t *)"<p>only <b>html</b></p>", 23, NULL, 0 };
  struct clip_in hin = { .fmts = &h, .n_fmts = 1 };
  CHECK_INT(db_add_clip(db, &hin, &id), ADD_NEW);
  char *t = db_clip_text(db, id, NULL);
  CHECK_STR(t, "only html");
  free(t);
  CHECK_INT(list_total(db, "only html"), 1);

  /* HTML offered under the plain-text names too: text means the markup. */
  const char *markup = "<p>Hello <b>rich</b> world</p>";
  struct fmt_in both[] = {
    { MIME_TEXT, (const uint8_t *)markup, strlen(markup), NULL, 0 },
    { "text/html", (const uint8_t *)markup, strlen(markup), NULL, 0 },
  };
  struct clip_in bin = { .fmts = both, .n_fmts = 2 };
  CHECK_INT(db_add_clip(db, &bin, &id), ADD_NEW);
  r = db_row(db, id);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "preview")), "Hello rich world");
  cJSON_Delete(r);
  t = db_clip_text(db, id, NULL);
  CHECK_STR(t, "Hello rich world");
  free(t);
  /* A real plain alternative is left alone. */
  struct fmt_in real[] = {
    { MIME_TEXT, (const uint8_t *)"Hello rich world!", 17, NULL, 0 },
    { "text/html", (const uint8_t *)markup, strlen(markup), NULL, 0 },
  };
  struct clip_in rin = { .fmts = real, .n_fmts = 2 };
  CHECK_INT(db_add_clip(db, &rin, &id), ADD_NEW);
  t = db_clip_text(db, id, NULL);
  CHECK_STR(t, "Hello rich world!");
  free(t);
  db_close(db);
}

static void set_int(struct db *db, const char *k, int v)
{
  cJSON *n = cJSON_CreateNumber(v);
  const char *err;
  CHECK_INT(setting_set(db, k, n, &err), 0);
  cJSON_Delete(n);
}

static void test_retention(void)
{
  struct db *db = fresh("retention");
  const int64_t day = 86400000, now = 100 * day;
  int64_t old = add_text(db, "old", now - 10 * day, NULL);
  int64_t locked = add_text(db, "old but locked", now - 10 * day, NULL);
  add_text(db, "new", now - day, NULL);
  sqlite3_exec(db_handle(db), "UPDATE clips SET locked = 1 WHERE preview = 'old but locked'", NULL, NULL, NULL);

  /* Defaults: unlimited, nothing removed. */
  CHECK_INT(db_retention(db, now), 0);
  set_int(db, "max_age_days", 5);
  CHECK_INT(db_retention(db, now), 1);
  CHECK(!db_exists(db, old));
  CHECK(db_exists(db, locked));

  set_int(db, "max_age_days", 0);
  for (int i = 0; i < 5; i++) {
    char s[16];
    snprintf(s, sizeof s, "n%d", i);
    add_text(db, s, now - day + i, NULL);
  }
  /* 7 clips, one protected; keep 3 → the 4 oldest expendable ones go. */
  set_int(db, "max_clips", 3);
  CHECK_INT(db_retention(db, now), 4);
  CHECK_INT(list_total(db, NULL), 3);
  CHECK(db_exists(db, locked));

  /* Bad values are refused. */
  cJSON *neg = cJSON_CreateNumber(-1);
  const char *err = NULL;
  CHECK_INT(setting_set(db, "max_clips", neg, &err), -1);
  CHECK(err != NULL);
  CHECK_INT(setting_set(db, "no_such_setting", neg, &err), -1);
  cJSON_Delete(neg);
  cJSON *pos = cJSON_CreateString("sideways");
  CHECK_INT(setting_set(db, "popup_position", pos, &err), -1);
  cJSON_Delete(pos);
  db_close(db);
}

void run_tests(void)
{
  test_open_and_schema();
  test_add_dedupe_order();
  test_search();
  test_payload_roundtrip();
  test_image_and_html_only();
  test_retention();
}

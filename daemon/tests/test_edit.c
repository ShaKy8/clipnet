#include "harness.h"

#include <stdlib.h>
#include <unistd.h>

#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "settings.h"
#include "util.h"

static struct db *db;

static int64_t add(const char *text, int64_t at)
{
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)text, strlen(text), NULL, 0 };
  struct clip_in in = { .fmts = &f, .n_fmts = 1, .created_at = at };
  int64_t id = 0;
  db_add_clip(db, &in, &id);
  return id;
}

/* Previews of a list, joined with "|", for compact order checks. */
static char *order(int64_t group, const char *query)
{
  struct list_query q = { .group_id = group, .query = query, .limit = 100 };
  cJSON *r = db_list(db, &q);
  struct buf b = { 0 };
  cJSON *row;
  cJSON_ArrayForEach(row, cJSON_GetObjectItem(r, "rows")) {
    if (b.len) buf_append(&b, "|", 1);
    buf_append_str(&b, cJSON_GetStringValue(cJSON_GetObjectItem(row, "preview")));
  }
  cJSON_Delete(r);
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

#define ORDER(group, query, want) do { char *_o = order(group, query); CHECK_STR(_o, want); free(_o); } while (0)

static void test_titles_words_locks(void)
{
  const char *err = NULL;
  int64_t a = add("alpha", 1000), b = add("bravo", 2000);
  CHECK_INT(db_clip_set_title(db, a, "  My title  ", &err), 0);
  cJSON *r = db_row(db, a);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "title")), "My title");
  cJSON_Delete(r);
  CHECK_INT(db_clip_set_title(db, a, "   ", &err), 0);
  r = db_row(db, a);
  CHECK(cJSON_IsNull(cJSON_GetObjectItem(r, "title")));
  cJSON_Delete(r);

  CHECK_INT(db_clip_set_quick_paste(db, a, "sig", &err), 0);
  CHECK_INT(db_clip_set_quick_paste(db, b, "sig", &err), -1);
  CHECK(err && strstr(err, "already"));
  CHECK_INT(db_clip_set_quick_paste(db, b, "two words", &err), -1);
  /* The exact quick-paste word ranks first when searched. */
  ORDER(0, "sig", "alpha");
  CHECK_INT(db_clip_set_quick_paste(db, a, "", &err), 0);
  CHECK_INT(db_clip_set_quick_paste(db, b, "sig", &err), 0);

  CHECK_INT(db_clip_set_locked(db, a, true, &err), 0);
  r = db_row(db, a);
  CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "locked")));
  cJSON_Delete(r);
  CHECK_INT(db_clip_set_title(db, 999999, "x", &err), -1);
  CHECK_STR(err, "no such clip");
  db_delete(db, a);
  db_delete(db, b);
}

static void test_sticky(void)
{
  const char *err;
  int64_t a = add("s-a", 1000), b = add("s-b", 2000), c = add("s-c", 3000), d = add("s-d", 4000);
  (void)d;
  ORDER(0, "s-", "s-d|s-c|s-b|s-a");
  CHECK_INT(db_clip_set_sticky(db, a, STICKY_TOP, &err), 0);
  ORDER(0, "s-", "s-a|s-d|s-c|s-b");
  CHECK_INT(db_clip_set_sticky(db, b, STICKY_TOP, &err), 0);
  ORDER(0, "s-", "s-b|s-a|s-d|s-c");
  CHECK_INT(db_clip_set_sticky(db, c, STICKY_BOTTOM, &err), 0);
  ORDER(0, "s-", "s-b|s-a|s-c|s-d");
  int64_t new_order[] = { c, a, b };
  CHECK_INT(db_clip_reorder_sticky(db, new_order, 3, &err), 0);
  ORDER(0, "s-", "s-c|s-a|s-b|s-d");
  CHECK_INT(db_clip_set_sticky(db, a, STICKY_OFF, &err), 0);
  ORDER(0, "s-", "s-c|s-b|s-d|s-a");
  /* Sticky clips survive retention. */
  cJSON *one = cJSON_CreateNumber(1);
  setting_set(db, "max_clips", one, &err);
  cJSON_Delete(one);
  db_retention(db, now_ms());
  ORDER(0, "s-", "s-c|s-b");
  cJSON *zero = cJSON_CreateNumber(0);
  setting_set(db, "max_clips", zero, &err);
  cJSON_Delete(zero);
  db_delete(db, b);
  db_delete(db, c);
}

static void test_groups(void)
{
  const char *err;
  int64_t work = db_group_create(db, "Work", 0, &err);
  CHECK(work > 0);
  CHECK_INT(db_group_create(db, " Work ", 0, &err), -1);         /* same name, same level */
  CHECK(err && strstr(err, "already"));
  int64_t snippets = db_group_create(db, "Snippets", work, &err);
  CHECK(snippets > 0);
  CHECK(db_group_create(db, "Snippets", 0, &err) > 0);            /* same name elsewhere is fine */
  CHECK_INT(db_group_create(db, "  ", 0, &err), -1);
  CHECK_INT(db_group_create(db, "x", 424242, &err), -1);

  CHECK_INT(db_group_move(db, work, snippets, &err), -1);          /* into its own child */
  CHECK_INT(db_group_move(db, work, work, &err), -1);
  CHECK_INT(db_group_rename(db, snippets, "Code", &err), 0);

  int64_t g1 = add("grouped one", 1000), g2 = add("grouped two", 2000), loose = add("loose", 3000);
  CHECK_INT(db_clip_move(db, g1, work, &err), 0);
  CHECK_INT(db_clip_move(db, g2, snippets, &err), 0);
  CHECK_INT(db_clip_move(db, loose, 777777, &err), -1);
  ORDER(work, NULL, "grouped one");
  ORDER(snippets, NULL, "grouped two");
  /* History shows grouped clips too by default... */
  ORDER(0, "grouped", "grouped two|grouped one");
  /* ...unless the setting says otherwise. */
  cJSON *f = cJSON_CreateFalse();
  setting_set(db, "show_grouped_in_history", f, &err);
  cJSON_Delete(f);
  ORDER(0, "grouped", "");
  cJSON *t = cJSON_CreateTrue();
  setting_set(db, "show_grouped_in_history", t, &err);
  cJSON_Delete(t);

  cJSON *gl = db_groups_list(db);
  int found = 0;
  cJSON *g;
  cJSON_ArrayForEach(g, gl) {
    if ((int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(g, "id")) == work) {
      CHECK_INT(cJSON_GetNumberValue(cJSON_GetObjectItem(g, "count")), 1);
      found++;
    }
  }
  CHECK_INT(found, 1);
  cJSON_Delete(gl);

  /* Deleting keeps the clips (back in history), including sub-group clips. */
  int64_t *aff;
  size_t n;
  CHECK_INT(db_group_delete(db, work, false, &aff, &n, &err), 0);
  CHECK_INT(n, 2);
  free(aff);
  CHECK(!db_group_exists(db, snippets));
  CHECK(db_exists(db, g1) && db_exists(db, g2));
  cJSON *r = db_row(db, g2);
  CHECK(cJSON_IsNull(cJSON_GetObjectItem(r, "group_id")));
  cJSON_Delete(r);

  /* Cascade takes the clips with it. */
  int64_t tmp = db_group_create(db, "Temp", 0, &err);
  db_clip_move(db, g1, tmp, &err);
  CHECK_INT(db_group_delete(db, tmp, true, &aff, &n, &err), 0);
  CHECK_INT(n, 1);
  free(aff);
  CHECK(!db_exists(db, g1));
  db_delete(db, g2);
  db_delete(db, loose);
}

static void test_set_text(void)
{
  const char *err;
  static const uint8_t png[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0,
  };
  struct fmt_in f[] = {
    { MIME_TEXT, (const uint8_t *)"old text", 8, NULL, 0 },
    { "text/html", (const uint8_t *)"<b>old text</b>", 15, NULL, 0 },
    { "image/png", png, sizeof png, NULL, 0 },
  };
  struct clip_in in = { .fmts = f, .n_fmts = 3 };
  int64_t id;
  db_add_clip(db, &in, &id);
  db_clip_set_title(db, id, "keep me", &err);
  cJSON *before = db_get(db, id);
  char *blob = NULL;
  cJSON *fm;
  cJSON_ArrayForEach(fm, cJSON_GetObjectItem(before, "formats"))
    if (cJSON_GetObjectItem(fm, "path")) blob = xstrdup(cJSON_GetStringValue(cJSON_GetObjectItem(fm, "path")));
  cJSON_Delete(before);
  CHECK(blob && access(blob, F_OK) == 0);

  CHECK_INT(db_clip_set_text(db, id, "new text", &err), 0);
  cJSON *after = db_get(db, id);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(after, "text")), "new text");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(after, "kind")), "text");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(after, "title")), "keep me");
  CHECK_INT(cJSON_GetArraySize(cJSON_GetObjectItem(after, "formats")), 1);
  cJSON_Delete(after);
  CHECK(access(blob, F_OK) != 0); /* the image went with the old content */
  free(blob);
  ORDER(0, "new text", "new text");
  ORDER(0, "old text", "");

  int64_t other = add("taken", 5000);
  CHECK_INT(db_clip_set_text(db, id, "taken", &err), -1);
  CHECK(err && strstr(err, "already"));
  CHECK_INT(db_clip_set_text(db, id, "", &err), -1);
  db_delete(db, id);
  db_delete(db, other);
}

void run_tests(void)
{
  char *dir = xasprintf("%s/edit", test_tmpdir());
  db = db_open(dir);
  free(dir);
  if (!db) { CHECK(!"db_open"); return; }
  test_titles_words_locks();
  test_sticky();
  test_groups();
  test_set_text();
  db_close(db);
}

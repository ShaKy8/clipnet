#include "harness.h"

#include <stdlib.h>

#include "cJSON.h"
#include "db.h"
#include "hypr.h"
#include "mime.h"
#include "rules.h"
#include "util.h"

static struct db *db;

static int64_t set(const char *json, const char **err)
{
  cJSON *spec = cJSON_Parse(json);
  int64_t id = rules_set(db, 0, spec, err);
  cJSON_Delete(spec);
  return id;
}

void run_tests(void)
{
  char *dir = xasprintf("%s/rules", test_tmpdir());
  db = db_open(dir);
  free(dir);
  const char *err = NULL;

  /* Seeded password-manager exclusions, case-insensitive. */
  CHECK(rules_excluded(db, "org.keepassxc.KeePassXC"));
  CHECK(rules_excluded(db, "1Password"));
  CHECK(rules_excluded(db, "com.onepassword.OnePassword"));
  CHECK(rules_excluded(db, "Bitwarden"));
  CHECK(!rules_excluded(db, "firefox"));
  CHECK(!rules_excluded(db, NULL));

  /* exclude */
  int64_t ex = set("{\"action\":\"exclude\",\"match_app\":\"*Slack*\"}", &err);
  CHECK(ex > 0);
  CHECK(rules_excluded(db, "com.slack.Slack"));
  cJSON *spec = cJSON_Parse("{\"action\":\"exclude\",\"match_app\":\"*Slack*\",\"enabled\":false}");
  CHECK_INT(rules_set(db, ex, spec, &err), ex);
  cJSON_Delete(spec);
  CHECK(!rules_excluded(db, "com.slack.Slack")); /* disabled */

  /* skip_mime: drop images copied from a browser, keep the rest. */
  CHECK(set("{\"action\":\"skip_mime\",\"match_app\":\"*chromium*\",\"match_mime\":\"image/*\"}", &err) > 0);
  const char *offered[] = { "text/html", "image/png", "text/plain;charset=utf-8" };
  struct mime_plan p;
  mime_plan_build(&p, offered, 3);
  CHECK_INT(p.n_reads, 3);
  rules_filter_plan(db, "Chromium", &p);
  CHECK_INT(p.n_reads, 2);
  for (size_t i = 0; i < p.n_reads; i++) CHECK(strcmp(p.reads[i].canonical, "image/png"));
  mime_plan_build(&p, offered, 3);
  rules_filter_plan(db, "firefox", &p);
  CHECK_INT(p.n_reads, 3); /* other apps unaffected */
  /* A rule without an app applies to every app. */
  CHECK(set("{\"action\":\"skip_mime\",\"match_mime\":\"text/html\"}", &err) > 0);
  mime_plan_build(&p, offered, 3);
  rules_filter_plan(db, "firefox", &p);
  CHECK_INT(p.n_reads, 2);

  /* paste_keys: stored canonical, handed to Hyprland as mods + keysym. */
  CHECK(set("{\"action\":\"paste_keys\",\"match_app\":\"*kitty*\",\"arg\":{\"keys\":\"shift+ctrl+v\"}}", &err) > 0);
  CHECK(set("{\"action\":\"paste_keys\",\"match_app\":\"*xterm*\",\"arg\":{\"keys\":\"Shift+Insert\"}}", &err) > 0);
  struct paste_key *keys;
  size_t n = rules_paste_keys(db, &keys);
  CHECK_INT(n, 2);
  if (n == 2) {
    CHECK_STR(keys[0].pattern, "*kitty*");
    CHECK_STR(keys[0].mods, "CTRL SHIFT");
    CHECK_STR(keys[0].key, "V");
    CHECK_STR(keys[1].mods, "SHIFT");
    CHECK_STR(keys[1].key, "Insert");
  }
  rules_paste_keys_free(keys, n);

  /* to_group */
  int64_t g = db_group_create(db, "Chats", 0, &err);
  cJSON *gl = db_groups_list(db);
  const char *uuid = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(gl, 0), "uuid"));
  char *json = xasprintf("{\"action\":\"to_group\",\"match_app\":\"*signal*\",\"arg\":{\"group\":\"%s\"}}", uuid);
  CHECK(set(json, &err) > 0);
  free(json);
  cJSON_Delete(gl);
  CHECK_INT(rules_route_group(db, "org.signal.Signal"), g);
  CHECK_INT(rules_route_group(db, "firefox"), 0);

  /* Validation. */
  CHECK_INT(set("{\"action\":\"exclude\"}", &err), -1);
  CHECK_INT(set("{\"action\":\"skip_mime\",\"match_app\":\"x\"}", &err), -1);
  CHECK_INT(set("{\"action\":\"paste_keys\",\"match_app\":\"x\",\"arg\":{\"keys\":\"Ctrl+Nope\"}}", &err), -1);
  CHECK_INT(set("{\"action\":\"to_group\",\"match_app\":\"x\",\"arg\":{\"group\":\"not-a-uuid\"}}", &err), -1);
  CHECK_INT(set("{\"action\":\"launch_missiles\"}", &err), -1);

  /* Listing, reordering, deleting. */
  cJSON *list = rules_list(db);
  int count = cJSON_GetArraySize(list);
  CHECK(count >= 9);
  int64_t first = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "id"));
  int64_t last = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, count - 1), "id"));
  cJSON_Delete(list);
  int64_t order[] = { last, first };
  CHECK_INT(rules_reorder(db, order, 2, &err), 0);
  list = rules_list(db);
  /* The named rules lead, in the given order; the rest follow unchanged. */
  CHECK_INT(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "id")), last);
  CHECK_INT(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 1), "id")), first);
  CHECK_INT(cJSON_GetArraySize(list), count);
  cJSON_Delete(list);
  CHECK_INT(rules_delete(db, last, &err), 0);
  CHECK_INT(rules_delete(db, last, &err), -1);
  db_close(db);
}

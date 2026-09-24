#include "harness.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "script.h"
#include "util.h"

static struct app app;
static char *dir;

static void write_script(const char *name, const char *code)
{
  char *path = xasprintf("%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  fputs(code, f);
  fclose(f);
  free(path);
}

static void enable(const char *name)
{
  const char *err;
  CHECK_INT(scripts_enable(app.scripts, name, true, &err), 0);
}

static void disable(const char *name)
{
  const char *err;
  scripts_enable(app.scripts, name, false, &err);
}

static void on_copy(const char *text, struct copy_verdict *v)
{
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)text, strlen(text), NULL, 0 };
  scripts_on_copy(app.scripts, &f, 1, "org.test.App", "Test window", v);
}

static const char *error_of(const char *name)
{
  static char buf[512];
  cJSON *l = scripts_list(app.scripts);
  cJSON *s;
  buf[0] = 0;
  cJSON_ArrayForEach(s, l) {
    if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")), name)) {
      const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(s, "error"));
      snprintf(buf, sizeof buf, "%s", e ? e : "");
    }
  }
  cJSON_Delete(l);
  return buf;
}

static void test_verdicts(void)
{
  struct copy_verdict v;
  write_script("10-shape.lua",
               "function on_copy(clip)\n"
               "  if clip.text == 'skip me' then return false end\n"
               "  if clip.text:match('^%d%d%d%d%d%d$') then return false end\n"
               "  if clip.text:find('github.com') then\n"
               "    return { group = 'Code/Git', title = 'repo', sticky = true, locked = true }\n"
               "  end\n"
               "  if clip.source.app == 'org.test.App' and clip.text == 'upper' then return { text = 'UPPER' } end\n"
               "end\n");
  /* Disabled scripts never run. */
  on_copy("skip me", &v);
  CHECK(!v.skip);
  copy_verdict_free(&v);

  enable("10-shape.lua");
  on_copy("skip me", &v);
  CHECK(v.skip);
  copy_verdict_free(&v);
  on_copy("123456", &v);
  CHECK(v.skip);
  copy_verdict_free(&v);
  on_copy("git clone https://github.com/x/y", &v);
  CHECK(!v.skip);
  CHECK_STR(v.group, "Code/Git");
  CHECK_STR(v.title, "repo");
  CHECK_INT(v.sticky, 1);
  CHECK_INT(v.locked, 1);
  copy_verdict_free(&v);
  on_copy("upper", &v);
  CHECK_STR(v.text, "UPPER");
  copy_verdict_free(&v);
  on_copy("nothing special", &v);
  CHECK(!v.skip && !v.text && !v.title && !v.group && v.sticky == -1);
  copy_verdict_free(&v);

  /* Scripts chain in name order: 20-* sees 10-*'s text. */
  write_script("20-chain.lua", "function on_copy(clip) return { text = clip.text .. '!' } end\n");
  enable("20-chain.lua");
  on_copy("upper", &v);
  CHECK_STR(v.text, "UPPER!");
  copy_verdict_free(&v);
  disable("20-chain.lua");
  disable("10-shape.lua");
}

static void test_paste(void)
{
  write_script("30-paste.lua",
               "function on_paste(clip, target)\n"
               "  if target.app == 'kitty' then return (clip.text:gsub('%s+$', '')) end\n"
               "end\n");
  enable("30-paste.lua");
  char *r = scripts_on_paste(app.scripts, 0, "trailing   \n", 12, "kitty");
  CHECK_STR(r, "trailing");
  free(r);
  CHECK(scripts_on_paste(app.scripts, 0, "keep", 4, "firefox") == NULL);
  disable("30-paste.lua");
  CHECK(!scripts_have_paste(app.scripts));
}

static void test_sandbox(void)
{
  struct copy_verdict v;
  /* Each probe returns a title naming what it could reach. */
  write_script("40-sandbox.lua",
               "function on_copy(clip)\n"
               "  local found = {}\n"
               "  if io then found[#found+1] = 'io' end\n"
               "  if os.execute then found[#found+1] = 'os.execute' end\n"
               "  if os.getenv then found[#found+1] = 'os.getenv' end\n"
               "  if os.remove then found[#found+1] = 'os.remove' end\n"
               "  if require then found[#found+1] = 'require' end\n"
               "  if dofile then found[#found+1] = 'dofile' end\n"
               "  if loadfile then found[#found+1] = 'loadfile' end\n"
               "  if debug then found[#found+1] = 'debug' end\n"
               "  if package then found[#found+1] = 'package' end\n"
               "  if string.dump then found[#found+1] = 'string.dump' end\n"
               "  local ok = load('return 1')\n"
               "  if not ok then found[#found+1] = 'text load broken' end\n"
               "  local bc = load('\\27Lua\\84\\0')\n"
               "  if bc then found[#found+1] = 'bytecode' end\n"
               "  return { title = 'found:' .. table.concat(found, ',') }\n"
               "end\n");
  enable("40-sandbox.lua");
  on_copy("x", &v);
  CHECK_STR(v.title, "found:");
  copy_verdict_free(&v);
  disable("40-sandbox.lua");

  /* An endless loop is stopped, and the copy goes on unchanged. */
  write_script("50-loop.lua", "function on_copy(clip) while true do end end\n");
  enable("50-loop.lua");
  int64_t t0 = mono_ms();
  on_copy("x", &v);
  int64_t took = mono_ms() - t0;
  CHECK(took >= 90 && took < 1000);
  CHECK(!v.skip && !v.text);
  CHECK(strstr(error_of("50-loop.lua"), "took longer"));
  copy_verdict_free(&v);
  disable("50-loop.lua");

  /* A memory bomb fails inside the script, not in the daemon. */
  write_script("60-memory.lua", "function on_copy(clip) local s = string.rep('x', 200 * 1024 * 1024) return { title = 'no' } end\n");
  enable("60-memory.lua");
  on_copy("x", &v);
  CHECK(!v.title);
  CHECK(strstr(error_of("60-memory.lua"), "memory"));
  copy_verdict_free(&v);
  disable("60-memory.lua");

  /* Bad return values and load errors are reported, never fatal. */
  write_script("70-bad.lua", "function on_copy(clip) return 42 end\n");
  enable("70-bad.lua");
  on_copy("x", &v);
  CHECK(strstr(error_of("70-bad.lua"), "must return"));
  copy_verdict_free(&v);
  write_script("80-syntax.lua", "function on_copy(clip) return end end end\n");
  CHECK(strstr(error_of("80-syntax.lua"), "80-syntax.lua"));
  write_script("81-empty.lua", "local x = 1\n");
  CHECK(strstr(error_of("81-empty.lua"), "neither"));
  disable("70-bad.lua");
}

static void test_reload_and_list(void)
{
  struct copy_verdict v;
  write_script("90-reload.lua", "function on_copy(clip) return { title = 'one' } end\n");
  enable("90-reload.lua");
  on_copy("x", &v);
  CHECK_STR(v.title, "one");
  copy_verdict_free(&v);
  sleep(1); /* mtime granularity */
  write_script("90-reload.lua", "function on_copy(clip) return { title = 'two, longer' } end\n");
  on_copy("x", &v);
  CHECK_STR(v.title, "two, longer");
  copy_verdict_free(&v);

  cJSON *l = scripts_list(app.scripts);
  bool found = false;
  cJSON *s;
  cJSON_ArrayForEach(s, l) {
    if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s, "name")), "90-reload.lua")) {
      found = cJSON_IsTrue(cJSON_GetObjectItem(s, "enabled")) &&
              !strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(s, "hooks"), 0)), "on_copy");
    }
  }
  CHECK(found);
  cJSON_Delete(l);

  /* A removed file disappears from the list and stops running. */
  char *path = xasprintf("%s/90-reload.lua", dir);
  unlink(path);
  free(path);
  on_copy("x", &v);
  CHECK(!v.title);
  copy_verdict_free(&v);
  const char *err;
  CHECK_INT(scripts_enable(app.scripts, "nope.lua", true, &err), -1);
}

static void test_dry_run(void)
{
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)"https://x.test/?utm_source=a&id=7", 34, NULL, 0 };
  struct clip_in in = { .fmts = &f, .n_fmts = 1, .source_app = "firefox" };
  int64_t id;
  db_add_clip(app.db, &in, &id);
  write_script("95-dry.lua",
               "function on_copy(clip)\n"
               "  clipnet.log('seen', clip.source.app, #clip.mimes)\n"
               "  return { text = (clip.text:gsub('utm_[%w_]+=[^&]*&?', '')) }\n"
               "end\n");
  const char *err = NULL;
  /* Works while disabled: testing comes before enabling. */
  cJSON *r = scripts_test(app.scripts, "95-dry.lua", "on_copy", id, &err);
  CHECK(r != NULL);
  cJSON *res = cJSON_GetObjectItem(r, "result");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(res, "text")), "https://x.test/?id=7");
  CHECK_STR(cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "log"), 0)), "seen firefox 1");
  CHECK(cJSON_IsNull(cJSON_GetObjectItem(r, "error")) || !cJSON_GetObjectItem(r, "error"));
  cJSON_Delete(r);
  r = scripts_test(app.scripts, "95-dry.lua", "on_paste", id, &err);
  CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "error")), "this script has no on_paste");
  cJSON_Delete(r);
  CHECK(scripts_test(app.scripts, "95-dry.lua", "on_copy", 999999, &err) == NULL);
}

/* The bundled examples, installed the way the Settings button does it. */
static void test_examples(void)
{
  const char *err = NULL;
  cJSON *r = scripts_install_examples(app.scripts, "../examples/scripts", &err);
  CHECK(r != NULL);
  CHECK_INT(cJSON_GetArraySize(cJSON_GetObjectItem(r, "added")), 4);
  cJSON_Delete(r);
  /* A second install never overwrites (the user may have edited them). */
  r = scripts_install_examples(app.scripts, "../examples/scripts", &err);
  CHECK_INT(cJSON_GetArraySize(cJSON_GetObjectItem(r, "added")), 0);
  cJSON_Delete(r);
  CHECK_STR(error_of("strip-tracking.lua"), "");

  struct copy_verdict v;
  enable("strip-tracking.lua");
  on_copy("https://shop.example/item?id=7&utm_source=news&utm_medium=mail&fbclid=abc#top", &v);
  CHECK_STR(v.text, "https://shop.example/item?id=7#top");
  copy_verdict_free(&v);
  on_copy("https://example.com/?utm_source=x", &v);
  CHECK_STR(v.text, "https://example.com/");
  copy_verdict_free(&v);
  on_copy("see https://example.com/?utm_source=x", &v); /* not just a URL */
  CHECK(!v.text);
  copy_verdict_free(&v);
  on_copy("https://example.com/?page=2", &v);
  CHECK(!v.text);
  copy_verdict_free(&v);
  disable("strip-tracking.lua");

  enable("skip-otp.lua");
  on_copy("482913", &v);
  CHECK(v.skip);
  copy_verdict_free(&v);
  on_copy(" 1234 ", &v);
  CHECK(v.skip);
  copy_verdict_free(&v);
  on_copy("123", &v);
  CHECK(!v.skip);
  copy_verdict_free(&v);
  on_copy("1234567890", &v);
  CHECK(!v.skip);
  copy_verdict_free(&v);
  disable("skip-otp.lua");

  enable("git-to-group.lua");
  on_copy("git clone https://github.com/sabrogden/Ditto", &v);
  CHECK_STR(v.group, "Code/Git");
  copy_verdict_free(&v);
  on_copy("git@github.com:example/project.git", &v);
  CHECK_STR(v.group, "Code/Git");
  copy_verdict_free(&v);
  on_copy("https://github.com/sabrogden/Ditto", &v);
  CHECK(!v.group);
  copy_verdict_free(&v);
  disable("git-to-group.lua");

  enable("terminal-safe-paste.lua");
  char *out = scripts_on_paste(app.scripts, 0, "ls -la\n", 7, "com.mitchellh.ghostty");
  CHECK_STR(out, "ls -la");
  free(out);
  CHECK(scripts_on_paste(app.scripts, 0, "ls -la\n", 7, "firefox") == NULL);
  CHECK(scripts_on_paste(app.scripts, 0, "no newline", 10, "kitty") == NULL);
  disable("terminal-safe-paste.lua");
}

void run_tests(void)
{
  char *data = xasprintf("%s/script-data", test_tmpdir());
  dir = xasprintf("%s/scripts", test_tmpdir());
  app.db = db_open(data);
  app.scripts = scripts_new(&app, dir);
  test_verdicts();
  test_paste();
  test_sandbox();
  test_reload_and_list();
  test_dry_run();
  test_examples();
  scripts_free(app.scripts);
  db_close(app.db);
  free(data);
  free(dir);
}

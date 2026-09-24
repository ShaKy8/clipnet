/*
 * How CLIP//NET behaves with a big history: fills a throwaway database with
 * N realistic clips (default 50000) and times what the popup does.
 *
 *   bench [N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "util.h"

static const char *words[] = {
  "clipboard", "history", "paste", "select", "users", "where", "function", "return", "https", "example",
  "omarchy", "hyprland", "ghostty", "config", "server", "request", "token", "search", "document", "meeting",
  "invoice", "address", "password", "commit", "branch", "deploy", "error", "warning", "kernel", "package",
};

static double ms_since(int64_t t0) { return (double)(mono_ms() - t0); }

static double time_list(struct db *db, const char *query, int reps)
{
  int64_t t0 = mono_ms();
  for (int i = 0; i < reps; i++) {
    struct list_query q = { .query = query, .limit = 300 };
    cJSON_Delete(db_list(db, &q));
  }
  return ms_since(t0) / reps;
}

int main(int argc, char **argv)
{
  int n = argc > 1 ? atoi(argv[1]) : 50000;
  char tmpl[] = "/tmp/clipnet-bench-XXXXXX";
  const char *base = getenv("TMPDIR");
  char *dir = base && *base ? xasprintf("%s/clipnet-bench-XXXXXX", base) : xstrdup(tmpl);
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
  log_threshold = LOG_ERR;
  struct db *db = db_open(dir);
  if (!db) return 1;

  srand(42);
  int64_t t0 = mono_ms();
  int64_t now = now_ms();
  struct buf text = { 0 };
  for (int i = 0; i < n; i++) {
    text.len = 0;
    int nw = 3 + rand() % 40; /* 3–43 words: commands to paragraphs */
    for (int w = 0; w < nw; w++) buf_printf(&text, "%s%s", w ? " " : "", words[rand() % ARRAY_LEN(words)]);
    buf_printf(&text, " #%d", i);
    struct fmt_in f = { MIME_TEXT, text.data, text.len, NULL, 0 };
    struct clip_in in = { .fmts = &f, .n_fmts = 1, .created_at = now - (int64_t)(n - i) * 60000 };
    if (db_add_clip(db, &in, NULL) == ADD_ERR) { fprintf(stderr, "insert failed at %d\n", i); return 1; }
  }
  double insert_total = ms_since(t0);
  buf_free(&text);

  printf("clips: %d\n", n);
  printf("insert (capture path):   %.2f ms/clip\n", insert_total / n);
  printf("open popup (list 300):   %.2f ms\n", time_list(db, NULL, 20));
  printf("search \"hyprland\":        %.2f ms\n", time_list(db, "hyprland", 20));
  printf("search \"users where\":     %.2f ms\n", time_list(db, "users where", 20));
  printf("search \"#4999\" (rare):    %.2f ms\n", time_list(db, "#4999", 20));
  printf("search \"xy\" (2 chars):    %.2f ms\n", time_list(db, "xy", 5));
  printf("search \"no-such-thing\":   %.2f ms\n", time_list(db, "no-such-thing", 20));
  t0 = mono_ms();
  for (int i = 0; i < 100; i++) db_id_at_position(db, i % 10);
  printf("Ctrl+N position lookup:  %.3f ms\n", ms_since(t0) / 100);
  t0 = mono_ms();
  db_retention(db, now_ms());
  printf("retention pass:          %.2f ms\n", ms_since(t0));
  cJSON *s = db_stats(db);
  printf("database size:           %.1f MB\n", cJSON_GetNumberValue(cJSON_GetObjectItem(s, "db_bytes")) / 1048576.0);
  cJSON_Delete(s);

  db_close(db);
  if (getenv("BENCH_KEEP")) {
    printf("kept: %s\n", dir);
    free(dir);
    return 0;
  }
  char *cmd = xasprintf("rm -rf '%s'", dir);
  if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
  free(cmd);
  free(dir);
  return 0;
}

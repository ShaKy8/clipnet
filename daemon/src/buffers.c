#include "buffers.h"

#include <sqlite3.h>
#include <stdlib.h>

#include "app.h"
#include "capture.h"
#include "cJSON.h"
#include "db.h"
#include "hypr.h"
#include "loop.h"
#include "rules.h"
#include "serve.h"
#include "settings.h"
#include "util.h"

/* How long to wait for the copied selection to arrive: the keystroke itself
 * may wait up to 600 ms for the hotkey's modifiers to be released. */
#define COPY_WAIT_MS 2000
/* After the target app first reads a buffer paste, it may still be reading
 * other formats; give it this long before putting the old clip back. */
#define RESTORE_AFTER_READ_MS 300
/* If the app never reads at all, restore anyway after this. */
#define RESTORE_FALLBACK_MS 3000

struct buffers {
  struct app *app;
  /* A copy in flight. */
  int copy_slot;
  int64_t copy_prev;
  /* A paste in flight. */
  int64_t paste_clip;
  int64_t paste_prev;
  struct loop_timer *restore_timer;
};

struct buffers *buffers_new(struct app *app)
{
  struct buffers *b = xcalloc(1, sizeof *b);
  b->app = app;
  return b;
}

void buffers_free(struct buffers *b)
{
  if (!b) return;
  loop_timer_cancel(b->app->loop, b->restore_timer);
  free(b);
}

static int set_slot(struct buffers *b, int slot, int64_t clip)
{
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(b->app->db),
                         "INSERT INTO copy_buffers(slot, clip_id) VALUES (?, ?)"
                         " ON CONFLICT(slot) DO UPDATE SET clip_id = excluded.clip_id",
                         -1, &st, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(st, 1, slot);
  sqlite3_bind_int64(st, 2, clip);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  return rc;
}

static int64_t slot_clip(struct buffers *b, int slot)
{
  sqlite3_stmt *st;
  int64_t id = 0;
  if (sqlite3_prepare_v2(db_handle(b->app->db), "SELECT clip_id FROM copy_buffers WHERE slot = ?", -1, &st, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_int(st, 1, slot);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  return id;
}

static bool valid_slot(int slot, const char **err)
{
  if (slot >= 1 && slot <= BUFFER_SLOTS) return true;
  *err = "copy buffers are 1, 2 and 3";
  return false;
}

static void restore(struct buffers *b, int64_t prev, int64_t expect_current)
{
  struct app *app = b->app;
  /* Only if nothing else took the clipboard in the meantime. */
  if (!prev || serve_current(app->serve) != expect_current || !db_exists(app->db, prev)) return;
  serve_clip(app->serve, prev, SERVE_ALL);
}

static void on_copied(void *ctx, int64_t clip)
{
  struct buffers *b = ctx;
  int slot = b->copy_slot;
  int64_t prev = b->copy_prev;
  b->copy_slot = 0;
  if (!slot) return;
  if (set_slot(b, slot, clip) < 0) {
    log_err("could not store copy buffer %d", slot);
    return;
  }
  log_debug("copy buffer %d holds clip %lld", slot, (long long)clip);
  cJSON *d = cJSON_CreateObject();
  cJSON_AddNumberToObject(d, "slot", slot);
  app_emit(b->app, "buffers.changed", d);
  /* The copy went into the buffer; the clipboard goes back to what it was.
   * capture made `clip` current, and serve does not own it, so restore
   * unconditionally unless the previous clip is the same one. */
  if (prev && prev != clip && db_exists(b->app->db, prev)) serve_clip(b->app->serve, prev, SERVE_ALL);
}

int buffers_copy(struct buffers *b, int slot, bool cut, const char **err)
{
  struct app *app = b->app;
  if (!valid_slot(slot, err)) return -1;
  if (!app->hypr) { *err = "copy buffers need Hyprland"; return -1; }
  if (app_paused(app)) { *err = "recording is paused"; return -1; }
  b->copy_slot = slot;
  b->copy_prev = capture_current_clip(app->capture);
  capture_expect(app->capture, COPY_WAIT_MS, on_copied, b);
  /* Terminals copy with Ctrl+Insert (Ctrl+C would interrupt), as Omarchy's
   * universal copy does; there is no cut in a terminal. */
  int delay = (int)setting_int(app->db, "paste_delay_ms");
  if (hypr_send_keys(app->hypr, delay, "CTRL", cut ? "X" : "C", "CTRL", "Insert") < 0) {
    b->copy_slot = 0;
    *err = "Hyprland refused the copy keystroke";
    return -1;
  }
  return 0;
}

static void on_restore(void *ctx)
{
  struct buffers *b = ctx;
  b->restore_timer = NULL;
  restore(b, b->paste_prev, b->paste_clip);
  b->paste_clip = b->paste_prev = 0;
}

static void on_read(void *ctx)
{
  struct buffers *b = ctx;
  loop_timer_cancel(b->app->loop, b->restore_timer);
  b->restore_timer = loop_timer(b->app->loop, RESTORE_AFTER_READ_MS, on_restore, b);
}

int buffers_paste(struct buffers *b, int slot, const char **err)
{
  struct app *app = b->app;
  if (!valid_slot(slot, err)) return -1;
  if (!app->hypr || !app->wl) { *err = "copy buffers need Hyprland"; return -1; }
  int64_t clip = slot_clip(b, slot);
  if (!clip || !db_exists(app->db, clip)) {
    *err = "that copy buffer is empty";
    return -1;
  }
  /* A paste still waiting to restore finishes first, or its "previous clip"
   * would become this buffer. */
  if (b->restore_timer) {
    loop_timer_cancel(app->loop, b->restore_timer);
    on_restore(b);
  }
  int64_t prev = capture_current_clip(app->capture);
  if (serve_clip(app->serve, clip, SERVE_ALL) < 0) { *err = "could not put the buffer on the clipboard"; return -1; }
  b->paste_clip = clip;
  b->paste_prev = prev != clip ? prev : 0;
  serve_on_first_read(app->serve, on_read, b);
  b->restore_timer = loop_timer(app->loop, RESTORE_FALLBACK_MS, on_restore, b);

  struct paste_key *keys = NULL;
  size_t n = rules_paste_keys(app->db, &keys);
  int rc = hypr_send_paste(app->hypr, (int)setting_int(app->db, "paste_delay_ms"), keys, n);
  rules_paste_keys_free(keys, n);
  if (rc < 0) { *err = "Hyprland refused the paste keystroke"; return -1; }
  return 0;
}

cJSON *buffers_state(struct buffers *b)
{
  cJSON *arr = cJSON_CreateArray();
  for (int slot = 1; slot <= BUFFER_SLOTS; slot++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slot", slot);
    int64_t clip = slot_clip(b, slot);
    cJSON *row = clip ? db_row(b->app->db, clip) : NULL;
    cJSON_AddItemToObject(o, "clip", row ? row : cJSON_CreateNull());
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

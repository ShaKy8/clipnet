#include "hotkeys.h"

#include <ctype.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "app.h"
#include "buffers.h"
#include "cJSON.h"
#include "db.h"
#include "hypr.h"
#include "paste.h"
#include "util.h"
#include "wl.h"

#define APP_ID "clipnetd"
/* Our binds carry this prefix, so conflict checks can tell them apart. */
#define DESC_PREFIX "CLIP//NET: "

struct hotkeys {
  struct app *app;
};

static sqlite3 *H(struct hotkeys *hk) { return db_handle(hk->app->db); }

struct hotkeys *hotkeys_new(struct app *app)
{
  struct hotkeys *hk = xcalloc(1, sizeof *hk);
  hk->app = app;
  return hk;
}

void hotkeys_free(struct hotkeys *hk)
{
  free(hk);
}

static bool valid_action(const char *action)
{
  static const char *const actions[] = {
    "paste_clip", "paste_position", "buffer_copy", "buffer_cut", "buffer_paste", "pause_toggle",
  };
  for (size_t i = 0; i < ARRAY_LEN(actions); i++)
    if (!strcmp(action, actions[i])) return true;
  return false;
}

/* A short, content-free label: bind descriptions are visible in Omarchy's
 * keybinding list, so a clip's text never goes there unless the user gave
 * it a title. */
static char *label_for(struct hotkeys *hk, const char *action, const char *arg)
{
  if (!strcmp(action, "paste_position")) return xasprintf("paste history item %s", arg);
  if (!strcmp(action, "buffer_copy")) return xasprintf("copy into buffer %s", arg);
  if (!strcmp(action, "buffer_cut")) return xasprintf("cut into buffer %s", arg);
  if (!strcmp(action, "buffer_paste")) return xasprintf("paste buffer %s", arg);
  if (!strcmp(action, "pause_toggle")) return xstrdup("pause or resume recording");
  sqlite3_stmt *st;
  char *out = NULL;
  if (sqlite3_prepare_v2(H(hk), "SELECT id, title FROM clips WHERE uuid = ?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, arg, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
      const char *title = (const char *)sqlite3_column_text(st, 1);
      out = title ? xasprintf("paste “%s”", title) : xasprintf("paste clip %lld", (long long)sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
  }
  return out ? out : xstrdup("paste a deleted clip");
}

/* What already holds these keys in Hyprland, other than our own binds.
 * Returns a malloc'd description or NULL. */
static char *hypr_conflict(struct hotkeys *hk, const struct accel *a, cJSON *binds)
{
  cJSON *b;
  const cJSON *digit_guess = NULL;
  bool digit = a->key[0] >= '0' && a->key[0] <= '9' && !a->key[1];
  cJSON_ArrayForEach(b, binds) {
    const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(b, "description"));
    if (desc && !strncmp(desc, DESC_PREFIX, strlen(DESC_PREFIX))) continue;
    uint32_t mods = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(b, "modmask"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(b, "key"));
    if (key && *key && accel_matches_hypr(a, mods, key)) {
      const char *what = desc && *desc ? desc : cJSON_GetStringValue(cJSON_GetObjectItem(b, "dispatcher"));
      return xstrdup(what ? what : "another binding");
    }
    /* Binds made by keycode ("code:10", how Omarchy binds its digit keys)
     * are reported without a key name. For a digit, assume the worst when
     * the modifiers match, and name the bind that mentions that digit. */
    if (digit && key && !*key && (mods & (ACCEL_SHIFT | ACCEL_CTRL | ACCEL_ALT | ACCEL_SUPER)) == a->mods) {
      const char *want = a->key[0] == '0' ? "10" : a->key;
      if (desc) {
        const char *p = strstr(desc, want);
        size_t wl = strlen(want);
        if (p && (p == desc || !isdigit((unsigned char)p[-1])) && !isdigit((unsigned char)p[wl])) digit_guess = b;
      }
      if (!digit_guess) digit_guess = b;
    }
  }
  if (digit_guess) {
    const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(digit_guess, "description"));
    return xstrdup(desc && *desc ? desc : "a keycode binding");
  }
  return NULL;
}

static void on_pressed(void *ctx, const char *name)
{
  struct hotkeys *hk = ctx;
  int64_t id = strtoll(name + 1, NULL, 10); /* "h<id>" */
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(H(hk), "SELECT action, arg FROM hotkeys WHERE id = ? AND enabled = 1", -1, &st, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_int64(st, 1, id);
  char *action = NULL, *arg = NULL;
  if (sqlite3_step(st) == SQLITE_ROW) {
    action = xstrdup((const char *)sqlite3_column_text(st, 0));
    const char *a = (const char *)sqlite3_column_text(st, 1);
    arg = a ? xstrdup(a) : NULL;
  }
  sqlite3_finalize(st);
  if (!action) return; /* deleted since it was bound */

  const char *err = NULL;
  int slot = arg ? atoi(arg) : 0;
  if (!strcmp(action, "buffer_copy") || !strcmp(action, "buffer_cut")) {
    if (buffers_copy(hk->app->buffers, slot, !strcmp(action, "buffer_cut"), &err) < 0) log_warn("hotkey: %s", err);
    goto done;
  }
  if (!strcmp(action, "buffer_paste")) {
    if (buffers_paste(hk->app->buffers, slot, &err) < 0) log_warn("hotkey: %s", err);
    goto done;
  }
  if (!strcmp(action, "pause_toggle")) {
    if (app_paused(hk->app)) app_resume(hk->app);
    else app_pause(hk->app, 0);
    goto done;
  }
  int64_t clip = 0;
  if (!strcmp(action, "paste_clip") && arg) clip = db_clip_id_by_uuid(hk->app->db, arg);
  else if (!strcmp(action, "paste_position") && arg) clip = db_id_at_position(hk->app->db, atoi(arg) - 1);
  if (clip) {
    struct paste_req req = { .ids = &clip, .n_ids = 1, .mode = SERVE_ALL, .send_keys = true };
    if (paste_run(hk->app, &req, &err) < 0) log_warn("hotkey h%lld: %s", (long long)id, err);
  }
done:
  free(action);
  free(arg);
}

void hotkeys_sync(struct hotkeys *hk)
{
  struct app *app = hk->app;
  if (!app->hypr || !wl_has_shortcuts(app->wl)) return;
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(H(hk), "SELECT id, accel, action, arg FROM hotkeys WHERE enabled = 1 ORDER BY id", -1, &st,
                         NULL) != SQLITE_OK)
    return;
  cJSON *binds = hypr_query(app->hypr, "binds");
  struct buf lua = { 0 };
  buf_append_str(&lua,
                 "do\n"
                 "for _, b in ipairs(_G.clipnetd_binds or {}) do pcall(function() b:remove() end) end\n"
                 "_G.clipnetd_binds = {}\n"
                 "local function add(keys, name, desc)\n"
                 "  local ok, b = pcall(hl.bind, keys, hl.dsp.global(\"" APP_ID ":\" .. name), { description = desc })\n"
                 "  if ok and b then table.insert(_G.clipnetd_binds, b) end\n"
                 "end\n");
  int n = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(st, 0);
    struct accel a;
    const char *err;
    if (!accel_parse((const char *)sqlite3_column_text(st, 1), &a, &err)) continue;
    char *conflict = hypr_conflict(hk, &a, binds);
    if (conflict) {
      /* Taken since it was set (a config change): leave the other bind be. */
      log_warn("hotkey h%lld skipped: its keys are bound to \"%s\"", (long long)id, conflict);
      free(conflict);
      continue;
    }
    char name[32], keys[96];
    snprintf(name, sizeof name, "h%lld", (long long)id);
    wl_shortcut_register(app->wl, APP_ID, name, "CLIP//NET hotkey", on_pressed, hk);
    accel_to_hypr(&a, keys);
    char *label = label_for(hk, (const char *)sqlite3_column_text(st, 2), (const char *)sqlite3_column_text(st, 3));
    char *desc = xasprintf(DESC_PREFIX "%s", label);
    buf_append_str(&lua, "add(");
    lua_quote(&lua, keys);
    buf_append_str(&lua, ", ");
    lua_quote(&lua, name);
    buf_append_str(&lua, ", ");
    lua_quote(&lua, desc);
    buf_append_str(&lua, ")\n");
    free(label);
    free(desc);
    n++;
  }
  sqlite3_finalize(st);
  cJSON_Delete(binds);
  buf_append_str(&lua, "end\n");
  /* The registrations must reach Hyprland before binds dispatch to them. */
  wl_flush(app->wl);
  if (hypr_eval(app->hypr, (char *)lua.data) == 0) log_debug("hotkeys: %d bound", n);
  buf_free(&lua);
}

void hotkeys_prune(struct hotkeys *hk)
{
  if (sqlite3_exec(H(hk), "DELETE FROM hotkeys WHERE action = 'paste_clip' AND arg NOT IN (SELECT uuid FROM clips)",
                   NULL, NULL, NULL) == SQLITE_OK &&
      sqlite3_changes(H(hk)) > 0)
    hotkeys_sync(hk);
}

cJSON *hotkeys_list(struct hotkeys *hk)
{
  cJSON *arr = cJSON_CreateArray();
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(H(hk), "SELECT id, accel, action, arg, enabled FROM hotkeys ORDER BY action, id", -1, &st,
                         NULL) != SQLITE_OK)
    return arr;
  cJSON *binds = hk->app->hypr ? hypr_query(hk->app->hypr, "binds") : NULL;
  while (sqlite3_step(st) == SQLITE_ROW) {
    cJSON *o = cJSON_CreateObject();
    const char *accel = (const char *)sqlite3_column_text(st, 1);
    const char *action = (const char *)sqlite3_column_text(st, 2);
    const char *arg = (const char *)sqlite3_column_text(st, 3);
    cJSON_AddNumberToObject(o, "id", (double)sqlite3_column_int64(st, 0));
    cJSON_AddStringToObject(o, "accel", accel);
    cJSON_AddStringToObject(o, "action", action);
    if (arg) cJSON_AddStringToObject(o, "arg", arg);
    else cJSON_AddNullToObject(o, "arg");
    cJSON_AddBoolToObject(o, "enabled", sqlite3_column_int(st, 4));
    char *label = label_for(hk, action, arg ? arg : "");
    cJSON_AddStringToObject(o, "label", label);
    free(label);
    struct accel a;
    const char *err;
    char *conflict = binds && accel_parse(accel, &a, &err) ? hypr_conflict(hk, &a, binds) : NULL;
    if (conflict) cJSON_AddStringToObject(o, "conflict", conflict);
    else cJSON_AddNullToObject(o, "conflict");
    free(conflict);
    cJSON_AddItemToArray(arr, o);
  }
  sqlite3_finalize(st);
  cJSON_Delete(binds);
  return arr;
}

int64_t hotkeys_set(struct hotkeys *hk, int64_t id, const char *accel_in, const char *action, const char *arg,
                    const char **err, char **err_buf)
{
  *err_buf = NULL;
  struct accel a;
  if (!accel_parse(accel_in, &a, err)) return -1;
  if (!action || !valid_action(action)) { *err = "unknown hotkey action"; return -1; }
  if (!strcmp(action, "pause_toggle")) arg = "";
  if (!arg) { *err = "the action needs an argument"; return -1; }
  if (!strncmp(action, "buffer_", 7) && (atoi(arg) < 1 || atoi(arg) > BUFFER_SLOTS || arg[1])) {
    *err = "copy buffers are 1, 2 and 3";
    return -1;
  }
  if (!strcmp(action, "paste_clip") && !db_clip_id_by_uuid(hk->app->db, arg)) { *err = "no such clip"; return -1; }
  if (!strcmp(action, "paste_position")) {
    int n = atoi(arg);
    if (n < 1 || n > 100) { *err = "positions are 1 to 100"; return -1; }
  }
  char canon[64];
  accel_format(&a, canon);

  if (hk->app->hypr) {
    cJSON *binds = hypr_query(hk->app->hypr, "binds");
    char *conflict = hypr_conflict(hk, &a, binds);
    cJSON_Delete(binds);
    if (conflict) {
      *err_buf = xasprintf("%s is already bound to “%s”", canon, conflict);
      *err = *err_buf;
      free(conflict);
      return -1;
    }
  }

  sqlite3 *h = H(hk);
  sqlite3_stmt *st;
  /* Another of our hotkeys on the same keys (the UNIQUE index would say so
   * too, but this names it). */
  if (sqlite3_prepare_v2(h, "SELECT id FROM hotkeys WHERE accel = ? AND id != ?", -1, &st, NULL) != SQLITE_OK) {
    *err = "database error";
    return -1;
  }
  sqlite3_bind_text(st, 1, canon, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 2, id);
  bool taken = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (taken) {
    *err_buf = xasprintf("%s is already one of your CLIP//NET hotkeys", canon);
    *err = *err_buf;
    return -1;
  }

  sqlite3_exec(h, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  bool ok = true;
  if (!strcmp(action, "paste_clip")) {
    /* One hotkey per clip, as in Ditto. */
    ok = sqlite3_prepare_v2(h, "DELETE FROM hotkeys WHERE action = 'paste_clip' AND arg = ? AND id != ?", -1, &st, NULL) ==
         SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(st, 1, arg, -1, SQLITE_STATIC);
      sqlite3_bind_int64(st, 2, id);
      ok = sqlite3_step(st) == SQLITE_DONE;
      sqlite3_finalize(st);
    }
  }
  if (ok) {
    const char *sql = id ? "UPDATE hotkeys SET accel = ?, action = ?, arg = ?, enabled = 1 WHERE id = ?"
                         : "INSERT INTO hotkeys(accel, action, arg) VALUES (?, ?, ?)";
    ok = sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(st, 1, canon, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
      sqlite3_bind_text(st, 3, arg, -1, SQLITE_STATIC);
      if (id) sqlite3_bind_int64(st, 4, id);
      ok = sqlite3_step(st) == SQLITE_DONE && (!id || sqlite3_changes(h) == 1);
      sqlite3_finalize(st);
    }
  }
  if (!ok) {
    sqlite3_exec(h, "ROLLBACK", NULL, NULL, NULL);
    *err = id ? "no such hotkey" : "database error";
    return -1;
  }
  if (!id) id = sqlite3_last_insert_rowid(h);
  sqlite3_exec(h, "COMMIT", NULL, NULL, NULL);
  hotkeys_sync(hk);
  return id;
}

int hotkeys_remove(struct hotkeys *hk, int64_t id, const char **err)
{
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(H(hk), "DELETE FROM hotkeys WHERE id = ?", -1, &st, NULL) != SQLITE_OK) {
    *err = "database error";
    return -1;
  }
  sqlite3_bind_int64(st, 1, id);
  bool done = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(H(hk)) == 1;
  sqlite3_finalize(st);
  if (!done) { *err = "no such hotkey"; return -1; }
  hotkeys_sync(hk);
  return 0;
}

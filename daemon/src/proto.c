#include "proto.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "cJSON.h"
#include "buffers.h"
#include "capture.h"
#include "db.h"
#include "export.h"
#include "hotkeys.h"
#include "hypr.h"
#include "import.h"
#include "ipc.h"
#include "mime.h"
#include "paste.h"
#include "rules.h"
#include "serve.h"
#include "settings.h"
#include "transform.h"
#include "util.h"

struct call {
  struct app *app;
  struct ipc_client *c;
  const cJSON *req;
  const cJSON *id;
};

static void ok(struct call *k, cJSON *result) { ipc_reply(k->c, k->id, result); }
static void fail(struct call *k, const char *code, const char *msg) { ipc_error(k->c, k->id, code, msg); }

static const cJSON *arg(struct call *k, const char *name)
{
  return cJSON_GetObjectItemCaseSensitive(k->req, name);
}

static const char *arg_str(struct call *k, const char *name)
{
  const cJSON *v = arg(k, name);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* An integer argument. Returns false (and replies) if present but not a
 * whole number, so every handler can bail out with one check. */
static bool arg_int(struct call *k, const char *name, int64_t def, int64_t *out)
{
  const cJSON *v = arg(k, name);
  if (!v || cJSON_IsNull(v)) { *out = def; return true; }
  if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble)) {
    char *m = xasprintf("'%s' must be a whole number", name);
    fail(k, "bad_request", m);
    free(m);
    return false;
  }
  *out = (int64_t)v->valuedouble;
  return true;
}

/* "ids": [1, 2] (or "id": 1). Returns the count; 0 means it already replied. */
static size_t arg_ids(struct call *k, int64_t **out)
{
  *out = NULL;
  const cJSON *ids = arg(k, "ids");
  const cJSON *one = arg(k, "id");
  if (cJSON_IsNumber(one) && !ids) {
    *out = xmalloc(sizeof **out);
    (*out)[0] = (int64_t)one->valuedouble;
    return 1;
  }
  if (!cJSON_IsArray(ids) || !cJSON_GetArraySize(ids)) {
    fail(k, "bad_request", "expected 'ids' (a non-empty array) or 'id'");
    return 0;
  }
  size_t n = 0;
  *out = xcalloc((size_t)cJSON_GetArraySize(ids), sizeof **out);
  const cJSON *it;
  cJSON_ArrayForEach(it, ids) {
    if (!cJSON_IsNumber(it)) {
      free(*out);
      *out = NULL;
      fail(k, "bad_request", "'ids' must hold numbers");
      return 0;
    }
    (*out)[n++] = (int64_t)it->valuedouble;
  }
  return n;
}

/* ---- session --------------------------------------------------------- */

static void op_hello(struct call *k)
{
  const cJSON *sub = arg(k, "subscribe");
  ipc_subscribe(k->c, cJSON_IsTrue(sub));
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "proto", CLIPNET_PROTO);
  cJSON_AddStringToObject(r, "version", CLIPNET_VERSION);
  cJSON_AddBoolToObject(r, "wayland", k->app->wl != NULL);
  cJSON_AddBoolToObject(r, "hyprland", k->app->hypr != NULL);
  cJSON_AddBoolToObject(r, "paused", app_paused(k->app));
  ok(k, r);
}

static void op_ping(struct call *k)
{
  ok(k, cJSON_CreateString("pong"));
}

/* Where the popup should open: the cursor and the monitor under it, in the
 * compositor's logical coordinates. */
static void op_show_context(struct call *k)
{
  /* About to paste: if the clipboard went empty behind our back (its owner
   * exited without a window closing), put the last clip back now. */
  capture_owner_may_have_left(k->app->capture);
  cJSON *r = cJSON_CreateObject();
  struct hypr *h = k->app->hypr;
  if (h) {
    cJSON *cur = hypr_query(h, "cursorpos");
    cJSON *mons = hypr_query(h, "monitors");
    double x = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "x"));
    double y = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "y"));
    if (cur) {
      cJSON *c = cJSON_AddObjectToObject(r, "cursor");
      cJSON_AddNumberToObject(c, "x", x);
      cJSON_AddNumberToObject(c, "y", y);
    }
    const cJSON *m, *hit = NULL, *focused = NULL;
    cJSON_ArrayForEach(m, mons) {
      double mx = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "x"));
      double my = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "y"));
      double sc = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "scale"));
      if (!(sc > 0)) sc = 1;
      /* Hyprland reports pixel size; positions are logical. */
      double mw = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "width")) / sc;
      double mh = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "height")) / sc;
      int tr = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "transform"));
      if (tr % 2 == 1) { double t = mw; mw = mh; mh = t; }
      if (cJSON_IsTrue(cJSON_GetObjectItem(m, "focused"))) focused = m;
      if (cur && x >= mx && x < mx + mw && y >= my && y < my + mh) {
        hit = m;
        cJSON *o = cJSON_AddObjectToObject(r, "monitor");
        cJSON_AddStringToObject(o, "name", cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")));
        cJSON_AddNumberToObject(o, "x", mx);
        cJSON_AddNumberToObject(o, "y", my);
        cJSON_AddNumberToObject(o, "width", mw);
        cJSON_AddNumberToObject(o, "height", mh);
        cJSON_AddNumberToObject(o, "scale", sc);
        break;
      }
    }
    if (!hit && focused) {
      cJSON *o = cJSON_AddObjectToObject(r, "monitor");
      cJSON_AddStringToObject(o, "name", cJSON_GetStringValue(cJSON_GetObjectItem(focused, "name")));
    }
    cJSON_Delete(cur);
    cJSON_Delete(mons);
    const char *cls = hypr_active_class(h);
    if (cls) cJSON_AddStringToObject(r, "app", cls);
    /* The popup rounds its corners like windows do. */
    cJSON *opt = hypr_query(h, "getoption decoration:rounding");
    cJSON *n = cJSON_GetObjectItem(opt, "int");
    if (cJSON_IsNumber(n)) cJSON_AddNumberToObject(r, "rounding", n->valuedouble);
    cJSON_Delete(opt);
  }
  ok(k, r);
}

/* ---- reading --------------------------------------------------------- */

static void op_list(struct call *k)
{
  int64_t group, offset, limit;
  if (!arg_int(k, "group", 0, &group) || !arg_int(k, "offset", 0, &offset) || !arg_int(k, "limit", 200, &limit)) return;
  struct list_query q = {
    .query = arg_str(k, "query"),
    .group_id = group,
    .all_groups = cJSON_IsTrue(arg(k, "all_groups")),
    .offset = (int)MAX(0, offset),
    .limit = (int)MAX(1, MIN(limit, 5000)),
  };
  ok(k, db_list(k->app->db, &q));
}

static void op_get(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  cJSON *r = db_get(k->app->db, id);
  if (!r) { fail(k, "not_found", "no such clip"); return; }
  ok(k, r);
}

static void op_stats(struct call *k)
{
  ok(k, db_stats(k->app->db));
}

/* ---- pasting --------------------------------------------------------- */

static bool mode_of(struct call *k, enum serve_mode *mode)
{
  const char *m = arg_str(k, "mode");
  if (!m || !strcmp(m, "normal")) { *mode = SERVE_ALL; return true; }
  if (!strcmp(m, "plain")) { *mode = SERVE_PLAIN; return true; }
  fail(k, "bad_request", "'mode' is \"normal\" or \"plain\"");
  return false;
}

static void do_paste(struct call *k, int64_t *ids, size_t n, bool keys)
{
  enum serve_mode mode;
  if (!mode_of(k, &mode)) return;
  struct paste_req pr = {
    .ids = ids, .n_ids = n, .mode = mode,
    .separator = arg_str(k, "separator"), .text = arg_str(k, "text"),
    .transform = arg_str(k, "transform"), .send_keys = keys,
  };
  const char *err = NULL;
  if (paste_run(k->app, &pr, &err) < 0) fail(k, "paste_failed", err);
  else ok(k, NULL);
}

static void op_paste_or_copy(struct call *k, bool keys)
{
  int64_t *ids;
  size_t n = arg_ids(k, &ids);
  if (!n) return;
  do_paste(k, ids, n, keys);
  free(ids);
}

static void op_position(struct call *k, bool keys)
{
  int64_t pos;
  if (!arg_int(k, "n", 1, &pos)) return;
  if (pos < 1) { fail(k, "bad_request", "positions start at 1"); return; }
  int64_t id = db_id_at_position(k->app->db, (int)pos - 1);
  if (!id) { fail(k, "not_found", "no clip at that position"); return; }
  do_paste(k, &id, 1, keys);
}

/* ---- changing -------------------------------------------------------- */

static void op_delete(struct call *k)
{
  int64_t *ids;
  size_t n = arg_ids(k, &ids);
  if (!n) return;
  int removed = 0;
  for (size_t i = 0; i < n; i++) {
    if (db_delete(k->app->db, ids[i]) == 0) {
      removed++;
      cJSON *d = cJSON_CreateObject();
      cJSON_AddNumberToObject(d, "id", (double)ids[i]);
      app_emit(k->app, "clip.deleted", d);
    }
  }
  free(ids);
  if (removed) hotkeys_prune(k->app->hotkeys);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "deleted", removed);
  ok(k, r);
}

/* ---- organising (Phase 2) -------------------------------------------- */

static void emit_row(struct call *k, int64_t id)
{
  app_emit_clip(k->app, "clip.updated", id);
}

/* {id, title?, quick_paste?, locked?, sticky?} — only the fields present. */
static void op_update(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  if (!db_exists(k->app->db, id)) { fail(k, "not_found", "no such clip"); return; }
  const char *err = NULL;
  const cJSON *v;
  if ((v = arg(k, "title")) && (cJSON_IsString(v) || cJSON_IsNull(v)))
    if (db_clip_set_title(k->app->db, id, cJSON_GetStringValue(v), &err) < 0) goto failed;
  if ((v = arg(k, "quick_paste")) && (cJSON_IsString(v) || cJSON_IsNull(v)))
    if (db_clip_set_quick_paste(k->app->db, id, cJSON_GetStringValue(v), &err) < 0) goto failed;
  if ((v = arg(k, "locked")) && cJSON_IsBool(v))
    if (db_clip_set_locked(k->app->db, id, cJSON_IsTrue(v), &err) < 0) goto failed;
  if ((v = arg(k, "sticky"))) {
    enum sticky_where w;
    if (cJSON_IsFalse(v) || cJSON_IsNull(v)) w = STICKY_OFF;
    else if (cJSON_IsTrue(v) || (cJSON_IsString(v) && !strcmp(v->valuestring, "top"))) w = STICKY_TOP;
    else if (cJSON_IsString(v) && !strcmp(v->valuestring, "bottom")) w = STICKY_BOTTOM;
    else { fail(k, "bad_request", "'sticky' is \"top\", \"bottom\" or false"); return; }
    if (db_clip_set_sticky(k->app->db, id, w, &err) < 0) goto failed;
  }
  emit_row(k, id);
  ok(k, db_row(k->app->db, id));
  return;
failed:
  emit_row(k, id);
  fail(k, "bad_value", err);
}

static void op_set_text(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  if (db_clip_set_text(k->app->db, id, arg_str(k, "text"), &err) < 0) { fail(k, "bad_value", err); return; }
  emit_row(k, id);
  ok(k, db_row(k->app->db, id));
}

static void op_move(struct call *k)
{
  int64_t *ids;
  size_t n = arg_ids(k, &ids);
  if (!n) return;
  int64_t group;
  if (!arg_int(k, "group", 0, &group)) { free(ids); return; }
  const char *err = NULL;
  size_t moved = 0;
  for (size_t i = 0; i < n; i++) {
    if (db_clip_move(k->app->db, ids[i], group, &err) == 0) {
      moved++;
      emit_row(k, ids[i]);
    }
  }
  free(ids);
  if (!moved && err) { fail(k, "bad_value", err); return; }
  app_emit(k->app, "groups.changed", NULL);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "moved", (double)moved);
  ok(k, r);
}

static void op_reorder_sticky(struct call *k)
{
  int64_t *ids;
  size_t n = arg_ids(k, &ids);
  if (!n) return;
  const char *err = NULL;
  int rc = db_clip_reorder_sticky(k->app->db, ids, n, &err);
  free(ids);
  if (rc < 0) { fail(k, "db_error", err); return; }
  app_emit(k->app, "clips.reset", NULL);
  ok(k, NULL);
}

static void op_groups_list(struct call *k)
{
  ok(k, db_groups_list(k->app->db));
}

static void op_groups_create(struct call *k)
{
  int64_t parent;
  if (!arg_int(k, "parent", 0, &parent)) return;
  const char *err = NULL;
  int64_t id = db_group_create(k->app->db, arg_str(k, "name"), parent, &err);
  if (id < 0) { fail(k, "bad_value", err); return; }
  app_emit(k->app, "groups.changed", NULL);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "id", (double)id);
  ok(k, r);
}

static void op_groups_rename(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  if (db_group_rename(k->app->db, id, arg_str(k, "name"), &err) < 0) { fail(k, "bad_value", err); return; }
  app_emit(k->app, "groups.changed", NULL);
  ok(k, NULL);
}

static void op_groups_move(struct call *k)
{
  int64_t id, parent;
  if (!arg_int(k, "id", 0, &id) || !arg_int(k, "parent", 0, &parent)) return;
  const char *err = NULL;
  if (db_group_move(k->app->db, id, parent, &err) < 0) { fail(k, "bad_value", err); return; }
  app_emit(k->app, "groups.changed", NULL);
  ok(k, NULL);
}

static void op_groups_delete(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  bool cascade = cJSON_IsTrue(arg(k, "cascade"));
  const char *err = NULL;
  int64_t *affected = NULL;
  size_t n = 0;
  if (db_group_delete(k->app->db, id, cascade, &affected, &n, &err) < 0) { fail(k, "bad_value", err); return; }
  free(affected);
  if (cascade) hotkeys_prune(k->app->hotkeys);
  app_emit(k->app, "groups.changed", NULL);
  app_emit(k->app, "clips.reset", NULL);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, cascade ? "deleted_clips" : "released_clips", (double)n);
  ok(k, r);
}

static void op_hotkeys_list(struct call *k)
{
  ok(k, hotkeys_list(k->app->hotkeys));
}

static void op_hotkeys_set(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  char *err_buf = NULL;
  const char *action = arg_str(k, "action");
  const char *hk_arg = arg_str(k, "arg");
  int64_t hid = hotkeys_set(k->app->hotkeys, id, arg_str(k, "accel"), action, hk_arg, &err, &err_buf);
  if (hid < 0) {
    fail(k, "bad_value", err);
    free(err_buf);
    return;
  }
  app_emit(k->app, "hotkeys.changed", NULL);
  if (action && !strcmp(action, "paste_clip") && hk_arg) {
    int64_t clip = db_clip_id_by_uuid(k->app->db, hk_arg);
    if (clip) emit_row(k, clip);
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "id", (double)hid);
  ok(k, r);
}

static void op_hotkeys_remove(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  if (hotkeys_remove(k->app->hotkeys, id, &err) < 0) { fail(k, "not_found", err); return; }
  app_emit(k->app, "hotkeys.changed", NULL);
  app_emit(k->app, "clips.reset", NULL);
  ok(k, NULL);
}

/* ---- copy buffers (Phase 3) ------------------------------------------ */

static void op_buffers_get(struct call *k)
{
  ok(k, buffers_state(k->app->buffers));
}

static void op_buffer_copy(struct call *k)
{
  int64_t slot;
  if (!arg_int(k, "slot", 0, &slot)) return;
  const char *err = NULL;
  if (buffers_copy(k->app->buffers, (int)slot, cJSON_IsTrue(arg(k, "cut")), &err) < 0) { fail(k, "bad_value", err); return; }
  ok(k, NULL);
}

static void op_buffer_paste(struct call *k)
{
  int64_t slot;
  if (!arg_int(k, "slot", 0, &slot)) return;
  const char *err = NULL;
  if (buffers_paste(k->app->buffers, (int)slot, &err) < 0) { fail(k, "paste_failed", err); return; }
  ok(k, NULL);
}

/* ---- rules (Phase 3) ------------------------------------------------- */

static void op_rules_list(struct call *k)
{
  ok(k, rules_list(k->app->db));
}

static void op_rules_set(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  int64_t rid = rules_set(k->app->db, id, k->req, &err);
  if (rid < 0) { fail(k, "bad_value", err); return; }
  app_emit(k->app, "rules.changed", NULL);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "id", (double)rid);
  ok(k, r);
}

static void op_rules_delete(struct call *k)
{
  int64_t id;
  if (!arg_int(k, "id", 0, &id)) return;
  const char *err = NULL;
  if (rules_delete(k->app->db, id, &err) < 0) { fail(k, "not_found", err); return; }
  app_emit(k->app, "rules.changed", NULL);
  ok(k, NULL);
}

static void op_rules_reorder(struct call *k)
{
  int64_t *ids;
  size_t n = arg_ids(k, &ids);
  if (!n) return;
  const char *err = NULL;
  int rc = rules_reorder(k->app->db, ids, n, &err);
  free(ids);
  if (rc < 0) { fail(k, "db_error", err); return; }
  app_emit(k->app, "rules.changed", NULL);
  ok(k, NULL);
}

static void op_transforms(struct call *k)
{
  size_t n;
  const struct transform_def *defs = transform_list(&n);
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; i < n; i++) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "id", defs[i].id);
    cJSON_AddStringToObject(t, "label", defs[i].label);
    cJSON_AddItemToArray(arr, t);
  }
  ok(k, arr);
}

static void op_export(struct call *k)
{
  const char *path = arg_str(k, "path");
  if (!path || path[0] != '/') { fail(k, "bad_request", "'path' must be an absolute file path"); return; }
  int64_t group;
  if (!arg_int(k, "group", 0, &group)) return;
  const char *err = NULL;
  cJSON *r = export_json(k->app->db, path, group, &err);
  if (!r) { fail(k, "export_failed", err); return; }
  ok(k, r);
}

static void op_create(struct call *k)
{
  const char *text = arg_str(k, "text");
  if (!text || !*text) { fail(k, "bad_request", "'text' must be a non-empty string"); return; }
  if (!utf8_valid((const uint8_t *)text, strlen(text))) { fail(k, "bad_request", "'text' is not valid UTF-8"); return; }
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)text, strlen(text), mime_text_default_aliases,
                      mime_text_default_alias_count };
  struct clip_in in = { .fmts = &f, .n_fmts = 1, .title = arg_str(k, "title") };
  int64_t id;
  enum add_result r = db_add_clip(k->app->db, &in, &id);
  if (r == ADD_ERR) { fail(k, "db_error", "could not store the clip"); return; }
  app_emit_clip(k->app, r == ADD_NEW ? "clip.added" : "clip.updated", id);
  cJSON *res = cJSON_CreateObject();
  cJSON_AddNumberToObject(res, "id", (double)id);
  cJSON_AddBoolToObject(res, "duplicate", r == ADD_DUP);
  ok(k, res);
}

/* ---- state and settings ---------------------------------------------- */

static void op_state(struct call *k)
{
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "paused", app_paused(k->app));
  cJSON_AddNumberToObject(r, "paused_until", (double)setting_int(k->app->db, "paused_until"));
  cJSON_AddNumberToObject(r, "current", (double)serve_current(k->app->serve));
  cJSON *s = db_stats(k->app->db);
  cJSON_AddItemToObject(r, "count", cJSON_DetachItemFromObject(s, "count"));
  cJSON_Delete(s);
  ok(k, r);
}

static void op_pause(struct call *k)
{
  int64_t minutes;
  if (!arg_int(k, "minutes", 0, &minutes)) return;
  if (minutes < 0 || minutes > 7 * 24 * 60) { fail(k, "bad_request", "'minutes' is 0 (until resumed) to 10080"); return; }
  app_pause(k->app, (int)minutes);
  ok(k, NULL);
}

static void op_resume(struct call *k)
{
  app_resume(k->app);
  ok(k, NULL);
}

static void op_settings_get(struct call *k)
{
  ok(k, settings_dump(k->app->db));
}

static void op_settings_set(struct call *k)
{
  const char *key = arg_str(k, "key");
  const cJSON *value = arg(k, "value");
  if (!key || !value) { fail(k, "bad_request", "expected 'key' and 'value'"); return; }
  const char *err = NULL;
  if (setting_set(k->app->db, key, value, &err) < 0) { fail(k, "bad_value", err); return; }
  cJSON *d = cJSON_CreateObject();
  cJSON_AddStringToObject(d, "key", key);
  cJSON_AddItemToObject(d, "value", cJSON_Duplicate(value, 1));
  app_emit(k->app, "settings.changed", d);
  ok(k, NULL);
}

/* ---- data ------------------------------------------------------------ */

static void op_import(struct call *k)
{
  const char *fmt = arg_str(k, "format");
  const char *path = arg_str(k, "path");
  if (fmt && !strcmp(fmt, "clipnet-json")) {
    if (!path || path[0] != '/') { fail(k, "bad_request", "'path' must be an absolute file path"); return; }
    const char *err = NULL;
    cJSON *r = import_json(k->app->db, path, &err);
    if (!r) { fail(k, "import_failed", err); return; }
    app_emit(k->app, "groups.changed", NULL);
    app_emit(k->app, "clips.reset", NULL);
    ok(k, r);
    return;
  }
  if (!fmt || strcmp(fmt, "omarchy")) { fail(k, "bad_request", "'format' is \"omarchy\" or \"clipnet-json\""); return; }
  char *def = xdg_path("XDG_STATE_HOME", ".local/state", "omarchy/clipboard-history.json");
  const char *err = NULL;
  cJSON *r = import_omarchy(k->app->db, path ? path : def, &err);
  free(def);
  if (!r) { fail(k, "import_failed", err); return; }
  app_emit(k->app, "clips.reset", NULL);
  ok(k, r);
}

static void op_retention(struct call *k)
{
  int n = db_retention(k->app->db, now_ms());
  if (n < 0) { fail(k, "db_error", "retention failed"); return; }
  if (n > 0) {
    hotkeys_prune(k->app->hotkeys);
    app_emit(k->app, "clips.reset", NULL);
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "removed", n);
  ok(k, r);
}

/* ---- dispatch -------------------------------------------------------- */

static void op_paste(struct call *k) { op_paste_or_copy(k, true); }
static void op_copy(struct call *k) { op_paste_or_copy(k, false); }
static void op_paste_position(struct call *k) { op_position(k, true); }
static void op_copy_position(struct call *k) { op_position(k, false); }

static const struct {
  const char *name;
  void (*fn)(struct call *k);
} ops[] = {
  { "hello", op_hello },
  { "ping", op_ping },
  { "show_context", op_show_context },
  { "list", op_list },
  { "get", op_get },
  { "stats", op_stats },
  { "paste", op_paste },
  { "copy", op_copy },
  { "paste_position", op_paste_position },
  { "copy_position", op_copy_position },
  { "delete", op_delete },
  { "create", op_create },
  { "state.get", op_state },
  { "pause", op_pause },
  { "resume", op_resume },
  { "settings.get", op_settings_get },
  { "settings.set", op_settings_set },
  { "import", op_import },
  { "retention.run", op_retention },
  { "update", op_update },
  { "set_text", op_set_text },
  { "move", op_move },
  { "reorder_sticky", op_reorder_sticky },
  { "groups.list", op_groups_list },
  { "groups.create", op_groups_create },
  { "groups.rename", op_groups_rename },
  { "groups.move", op_groups_move },
  { "groups.delete", op_groups_delete },
  { "hotkeys.list", op_hotkeys_list },
  { "hotkeys.set", op_hotkeys_set },
  { "hotkeys.remove", op_hotkeys_remove },
  { "transforms.list", op_transforms },
  { "export", op_export },
  { "buffers.get", op_buffers_get },
  { "buffer.copy", op_buffer_copy },
  { "buffer.paste", op_buffer_paste },
  { "rules.list", op_rules_list },
  { "rules.set", op_rules_set },
  { "rules.delete", op_rules_delete },
  { "rules.reorder", op_rules_reorder },
};

void proto_handle(struct app *app, struct ipc_client *c, const cJSON *req)
{
  struct call k = { app, c, req, cJSON_GetObjectItemCaseSensitive(req, "rid") };
  const char *op = arg_str(&k, "op");
  if (!op) { fail(&k, "bad_request", "missing 'op'"); return; }
  for (size_t i = 0; i < ARRAY_LEN(ops); i++) {
    if (!strcmp(ops[i].name, op)) {
      ops[i].fn(&k);
      return;
    }
  }
  fail(&k, "unknown_op", op);
}

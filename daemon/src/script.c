#include "script.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "cJSON.h"
#include "db.h"
#include "mime.h"
#include "util.h"

#define MEMORY_LIMIT (64u * 1024 * 1024)
#define CALL_MS 100
#define LOAD_MS 250
#define MAX_SCRIPT_BYTES (1024 * 1024)
#define MAX_LOG_LINES 20

/* Per-state limits, reachable from the allocator and the hook. */
struct limits {
  size_t used, max;
  int64_t deadline;
};

struct script {
  char *name;
  lua_State *L;
  struct limits lim;
  time_t mtime;
  off_t size;
  bool has_copy, has_paste;
  char *error;       /* load error, or the last run's error */
  /* Log lines from the current call, when being collected (dry runs). */
  cJSON *log;
  int log_lines;
  bool seen; /* still in the directory at the last scan */
  struct script *next;
};

struct scripts {
  struct app *app;
  char *dir;
  struct script *list;
};

/* ---- sandbox -------------------------------------------------------------- */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  struct limits *lim = ud;
  size_t old = ptr ? osize : 0; /* with ptr NULL, osize is a type tag */
  if (nsize == 0) {
    lim->used -= old;
    free(ptr);
    return NULL;
  }
  if (lim->used - old + nsize > lim->max) return NULL; /* Lua raises "not enough memory" */
  void *p = realloc(ptr, nsize);
  if (p) lim->used = lim->used - old + nsize;
  return p;
}

static void l_hook(lua_State *L, lua_Debug *ar)
{
  void *ud;
  lua_getallocf(L, &ud);
  struct limits *lim = ud;
  if (mono_ms() > lim->deadline) luaL_error(L, "took longer than %d ms", CALL_MS);
}

static struct script *self(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "clipnet.script");
  struct script *sc = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return sc;
}

/* clipnet.log(...) and print(...): strings joined by spaces. Collected in a
 * dry run; otherwise written to the daemon's log (so the user sees them in
 * journalctl), a few per call at most. */
static int l_log(lua_State *L)
{
  struct script *sc = self(L);
  if (!sc || sc->log_lines >= MAX_LOG_LINES) return 0;
  sc->log_lines++;
  /* Count first: luaL_buffinit pushes a placeholder onto the stack. */
  int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; i++) {
    if (i > 1) luaL_addchar(&b, ' ');
    luaL_tolstring(L, i, NULL);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  const char *msg = lua_tostring(L, -1);
  if (sc->log) cJSON_AddItemToArray(sc->log, cJSON_CreateString(msg));
  else log_info("script %s: %.500s", sc->name, msg);
  return 0;
}

/* load() that only accepts text: precompiled chunks can crash the VM. */
static int l_load(lua_State *L)
{
  size_t len;
  const char *chunk = luaL_checklstring(L, 1, &len);
  const char *name = luaL_optstring(L, 2, "=(load)");
  int status = luaL_loadbufferx(L, chunk, len, name, "t");
  if (status != LUA_OK) {
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
  if (!lua_isnone(L, 4)) {
    lua_pushvalue(L, 4);
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
  }
  return 1;
}

static void open_sandbox(lua_State *L)
{
  static const luaL_Reg libs[] = {
    { LUA_GNAME, luaopen_base },          { LUA_STRLIBNAME, luaopen_string },
    { LUA_TABLIBNAME, luaopen_table },    { LUA_MATHLIBNAME, luaopen_math },
    { LUA_UTF8LIBNAME, luaopen_utf8 },    { LUA_COLIBNAME, luaopen_coroutine },
  };
  for (size_t i = 0; i < ARRAY_LEN(libs); i++) {
    luaL_requiref(L, libs[i].name, libs[i].func, 1);
    lua_pop(L, 1);
  }
  /* Nothing that reaches the filesystem or runs bytecode. */
  static const char *const removed[] = { "dofile", "loadfile", "require" };
  for (size_t i = 0; i < ARRAY_LEN(removed); i++) {
    lua_pushnil(L);
    lua_setglobal(L, removed[i]);
  }
  lua_pushcfunction(L, l_load);
  lua_setglobal(L, "load");
  lua_pushcfunction(L, l_log);
  lua_setglobal(L, "print");
  lua_getglobal(L, LUA_STRLIBNAME);
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);

  /* os: the clock and calendar only. */
  luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 0);
  lua_newtable(L);
  static const char *const keep[] = { "time", "date", "clock", "difftime" };
  for (size_t i = 0; i < ARRAY_LEN(keep); i++) {
    lua_getfield(L, -2, keep[i]);
    lua_setfield(L, -2, keep[i]);
  }
  lua_setglobal(L, LUA_OSLIBNAME);
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushcfunction(L, l_log);
  lua_setfield(L, -2, "log");
  lua_pushstring(L, "1");
  lua_setfield(L, -2, "api");
  lua_setglobal(L, "clipnet");
}

/* ---- loading -------------------------------------------------------------- */

static void script_close(struct script *sc)
{
  if (sc->L) lua_close(sc->L);
  sc->L = NULL;
  sc->has_copy = sc->has_paste = false;
}

static void set_error(struct script *sc, const char *msg)
{
  free(sc->error);
  sc->error = msg ? xstrdup(msg) : NULL;
}

static char *path_of(struct scripts *s, const char *name)
{
  return xasprintf("%s/%s", s->dir, name);
}

/* (Re)load when the file changed. Returns true when the state is usable. */
static bool script_load(struct scripts *s, struct script *sc)
{
  char *path = path_of(s, sc->name);
  struct stat st;
  if (stat(path, &st) < 0) {
    free(path);
    script_close(sc);
    set_error(sc, "file not found");
    return false;
  }
  if (sc->L && st.st_mtime == sc->mtime && st.st_size == sc->size) {
    free(path);
    return true;
  }
  if (sc->L == NULL && sc->error && st.st_mtime == sc->mtime && st.st_size == sc->size) {
    free(path);
    return false; /* failed before and has not changed */
  }
  script_close(sc);
  set_error(sc, NULL);
  sc->mtime = st.st_mtime;
  sc->size = st.st_size;
  if (st.st_size > MAX_SCRIPT_BYTES) {
    free(path);
    set_error(sc, "script is larger than 1 MiB");
    return false;
  }
  FILE *f = fopen(path, "rb");
  free(path);
  if (!f) {
    set_error(sc, "cannot read the file");
    return false;
  }
  char *code = xmalloc((size_t)st.st_size + 1);
  size_t len = fread(code, 1, (size_t)st.st_size, f);
  fclose(f);

  sc->lim = (struct limits){ .max = MEMORY_LIMIT };
  sc->L = lua_newstate(l_alloc, &sc->lim);
  if (!sc->L) {
    free(code);
    set_error(sc, "out of memory");
    return false;
  }
  lua_State *L = sc->L;
  lua_pushlightuserdata(L, sc);
  lua_setfield(L, LUA_REGISTRYINDEX, "clipnet.script");
  open_sandbox(L);
  lua_sethook(L, l_hook, LUA_MASKCOUNT, 10000);

  char *chunkname = xasprintf("=%s", sc->name);
  sc->lim.deadline = mono_ms() + LOAD_MS;
  int rc = luaL_loadbufferx(L, code, len, chunkname, "t");
  free(chunkname);
  free(code);
  if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);
  if (rc != LUA_OK) {
    set_error(sc, lua_tostring(L, -1));
    script_close(sc);
    return false;
  }
  lua_getglobal(L, "on_copy");
  sc->has_copy = lua_isfunction(L, -1);
  lua_getglobal(L, "on_paste");
  sc->has_paste = lua_isfunction(L, -1);
  lua_pop(L, 2);
  if (!sc->has_copy && !sc->has_paste) set_error(sc, "defines neither on_copy nor on_paste");
  return true;
}

static bool valid_name(const char *name)
{
  size_t n = strlen(name);
  return n > 4 && name[0] != '.' && !strchr(name, '/') && !strcmp(name + n - 4, ".lua");
}

static struct script *find(struct scripts *s, const char *name, bool create)
{
  for (struct script *sc = s->list; sc; sc = sc->next)
    if (!strcmp(sc->name, name)) return sc;
  if (!create) return NULL;
  struct script *sc = xcalloc(1, sizeof *sc);
  sc->name = xstrdup(name);
  /* Keep the list sorted by name: that is the run order. */
  struct script **pp = &s->list;
  while (*pp && strcmp((*pp)->name, name) < 0) pp = &(*pp)->next;
  sc->next = *pp;
  *pp = sc;
  return sc;
}

/* Sync the list with the directory (new files appear, removed ones go). */
static void scan(struct scripts *s)
{
  DIR *d = opendir(s->dir);
  for (struct script *sc = s->list; sc; sc = sc->next) sc->seen = false;
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)))
      if (valid_name(de->d_name)) find(s, de->d_name, true)->seen = true;
    closedir(d);
  }
  for (struct script **pp = &s->list; *pp;) {
    struct script *sc = *pp;
    if (!sc->seen) {
      *pp = sc->next;
      script_close(sc);
      free(sc->error);
      free(sc->name);
      free(sc);
    } else {
      pp = &sc->next;
    }
  }
}

static bool enabled(struct scripts *s, const char *name)
{
  sqlite3_stmt *st;
  bool on = false;
  if (sqlite3_prepare_v2(db_handle(s->app->db), "SELECT enabled FROM scripts WHERE name = ?", -1, &st, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    on = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  return on;
}

/* ---- calling -------------------------------------------------------------- */

/* clip.get(mime): the bytes of one format, only during the call that
 * received the clip (a saved reference returns nil afterwards). */
struct call_ctx {
  const struct fmt_in *fmts;
  size_t n;
};

static int l_get(lua_State *L)
{
  const char *mime = luaL_checkstring(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "clipnet.call");
  struct call_ctx *ctx = lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (ctx) {
    for (size_t i = 0; i < ctx->n; i++) {
      if (!strcmp(ctx->fmts[i].mime, mime)) {
        lua_pushlstring(L, (const char *)ctx->fmts[i].data, ctx->fmts[i].len);
        return 1;
      }
    }
  }
  lua_pushnil(L);
  return 1;
}

static void push_clip(lua_State *L, const struct fmt_in *fmts, size_t n, const char *text, size_t text_len,
                      const char *app, const char *title, const char *clip_title)
{
  lua_newtable(L);
  const char **mimes = xcalloc(n ? n : 1, sizeof *mimes);
  size_t total = 0;
  for (size_t i = 0; i < n; i++) {
    mimes[i] = fmts[i].mime;
    total += fmts[i].len;
  }
  lua_pushstring(L, kind_name(mime_classify(mimes, n)));
  lua_setfield(L, -2, "kind");
  free(mimes);
  if (text) {
    lua_pushlstring(L, text, text_len);
    lua_setfield(L, -2, "text");
  }
  lua_pushinteger(L, (lua_Integer)total);
  lua_setfield(L, -2, "size");
  lua_createtable(L, (int)n, 0);
  for (size_t i = 0; i < n; i++) {
    lua_pushstring(L, fmts[i].mime);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  lua_setfield(L, -2, "mimes");
  lua_newtable(L);
  if (app) {
    lua_pushstring(L, app);
    lua_setfield(L, -2, "app");
  }
  if (title) {
    lua_pushstring(L, title);
    lua_setfield(L, -2, "title");
  }
  lua_setfield(L, -2, "source");
  if (clip_title) {
    lua_pushstring(L, clip_title);
    lua_setfield(L, -2, "title");
  }
  lua_pushcfunction(L, l_get);
  lua_setfield(L, -2, "get");
}

/* Call global `fn` with the values already pushed (nargs); one result left
 * on the stack on success. On failure records the error and returns false. */
static bool call(struct script *sc, const char *fn, int nargs, struct call_ctx *ctx)
{
  lua_State *L = sc->L;
  lua_getglobal(L, fn);
  lua_insert(L, -(nargs + 1));
  lua_pushlightuserdata(L, ctx);
  lua_setfield(L, LUA_REGISTRYINDEX, "clipnet.call");
  sc->lim.deadline = mono_ms() + CALL_MS;
  int rc = lua_pcall(L, nargs, 1, 0);
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "clipnet.call");
  if (rc != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    set_error(sc, msg ? msg : "error");
    if (!sc->log) log_warn("script %s: %.500s", sc->name, msg ? msg : "error");
    lua_pop(L, 1);
    return false;
  }
  set_error(sc, NULL);
  return true;
}

static char *opt_string_field(lua_State *L, const char *key)
{
  lua_getfield(L, -1, key);
  char *v = lua_type(L, -1) == LUA_TSTRING ? xstrdup(lua_tostring(L, -1)) : NULL;
  lua_pop(L, 1);
  return v;
}

static int opt_bool_field(lua_State *L, const char *key)
{
  lua_getfield(L, -1, key);
  int v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : -1;
  lua_pop(L, 1);
  return v;
}

/* Apply one on_copy result (top of stack) to the verdict. */
static void take_copy_result(struct script *sc, struct copy_verdict *v)
{
  lua_State *L = sc->L;
  int t = lua_type(L, -1);
  if (t == LUA_TNIL || (t == LUA_TBOOLEAN && lua_toboolean(L, -1))) {
    /* keep */
  } else if (t == LUA_TBOOLEAN) {
    v->skip = true;
  } else if (t == LUA_TTABLE) {
    char *s;
    if ((s = opt_string_field(L, "text"))) {
      if (utf8_valid((const uint8_t *)s, strlen(s)) && *s) { free(v->text); v->text = s; }
      else { free(s); set_error(sc, "on_copy returned text that is empty or not UTF-8; ignored"); }
    }
    if ((s = opt_string_field(L, "title"))) { free(v->title); v->title = s; }
    if ((s = opt_string_field(L, "group"))) { free(v->group); v->group = s; }
    int b;
    if ((b = opt_bool_field(L, "sticky")) >= 0) v->sticky = b;
    if ((b = opt_bool_field(L, "locked")) >= 0) v->locked = b;
  } else {
    set_error(sc, "on_copy must return nil, true, false or a table");
  }
  lua_pop(L, 1);
}

void copy_verdict_free(struct copy_verdict *v)
{
  free(v->text);
  free(v->title);
  free(v->group);
  memset(v, 0, sizeof *v);
  v->sticky = v->locked = -1;
}

static const struct fmt_in *text_fmt(const struct fmt_in *fmts, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (!strcmp(fmts[i].mime, MIME_TEXT)) return &fmts[i];
  return NULL;
}

static void run_copy(struct script *sc, const struct fmt_in *fmts, size_t n, const char *app, const char *title,
                     struct copy_verdict *out)
{
  const struct fmt_in *tf = text_fmt(fmts, n);
  const char *text = out->text ? out->text : tf ? (const char *)tf->data : NULL;
  size_t len = out->text ? strlen(out->text) : tf ? tf->len : 0;
  if (text && !utf8_valid((const uint8_t *)text, len)) text = NULL;
  struct call_ctx ctx = { fmts, n };
  push_clip(sc->L, fmts, n, text, len, app, title, out->title);
  if (call(sc, "on_copy", 1, &ctx)) take_copy_result(sc, out);
}

void scripts_on_copy(struct scripts *s, const struct fmt_in *fmts, size_t n, const char *source_app,
                     const char *source_title, struct copy_verdict *out)
{
  memset(out, 0, sizeof *out);
  out->sticky = out->locked = -1;
  scan(s);
  for (struct script *sc = s->list; sc && !out->skip; sc = sc->next) {
    if (!enabled(s, sc->name) || !script_load(s, sc) || !sc->has_copy) continue;
    sc->log_lines = 0;
    run_copy(sc, fmts, n, source_app, source_title, out);
  }
}

static char *run_paste(struct script *sc, const struct fmt_in *fmts, size_t nf, const char *text, size_t len,
                       const char *title, const char *target_app)
{
  lua_State *L = sc->L;
  struct call_ctx ctx = { fmts, nf };
  push_clip(L, fmts, nf, text, len, NULL, NULL, title);
  lua_newtable(L);
  if (target_app) {
    lua_pushstring(L, target_app);
    lua_setfield(L, -2, "app");
  }
  if (!call(sc, "on_paste", 2, &ctx)) return NULL;
  char *out = NULL;
  if (lua_type(L, -1) == LUA_TSTRING) {
    size_t n;
    const char *r = lua_tolstring(L, -1, &n);
    if (n && utf8_valid((const uint8_t *)r, n)) out = xstrndup(r, n);
    else set_error(sc, "on_paste returned text that is empty or not UTF-8; ignored");
  } else if (!lua_isnil(L, -1)) {
    set_error(sc, "on_paste must return nil or a string");
  }
  lua_pop(L, 1);
  return out;
}

/* Formats of a stored clip as fmt_in (borrowing the payload's memory). */
static struct fmt_in *payload_fmts(struct clip_payload *p)
{
  struct fmt_in *f = xcalloc(p->n_fmts ? p->n_fmts : 1, sizeof *f);
  for (size_t i = 0; i < p->n_fmts; i++)
    f[i] = (struct fmt_in){ p->fmts[i].mime, p->fmts[i].data, p->fmts[i].len, NULL, 0 };
  return f;
}

bool scripts_have_paste(struct scripts *s)
{
  scan(s);
  bool any = false;
  for (struct script *sc = s->list; sc; sc = sc->next)
    if (enabled(s, sc->name) && script_load(s, sc) && sc->has_paste) any = true;
  return any;
}

char *scripts_on_paste(struct scripts *s, int64_t clip_id, const char *text, size_t len, const char *target_app)
{
  if (!scripts_have_paste(s)) return NULL;

  struct clip_payload p = { 0 };
  struct fmt_in *fmts = NULL;
  size_t nf = 0;
  char *title = NULL;
  if (clip_id && db_load_payload(s->app->db, clip_id, &p) == 0) {
    fmts = payload_fmts(&p);
    nf = p.n_fmts;
    cJSON *row = db_row(s->app->db, clip_id);
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(row, "title"));
    title = t ? xstrdup(t) : NULL;
    cJSON_Delete(row);
  }
  char *current = NULL;
  for (struct script *sc = s->list; sc; sc = sc->next) {
    if (!enabled(s, sc->name) || !sc->L || !sc->has_paste) continue;
    sc->log_lines = 0;
    char *r = run_paste(sc, fmts, nf, current ? current : text, current ? strlen(current) : len, title, target_app);
    if (r) {
      free(current);
      current = r;
    }
  }
  free(title);
  free(fmts);
  clip_payload_free(&p);
  return current;
}

/* ---- management ----------------------------------------------------------- */

struct scripts *scripts_new(struct app *app, const char *dir)
{
  struct scripts *s = xcalloc(1, sizeof *s);
  s->app = app;
  s->dir = xstrdup(dir);
  mkdir_p(dir, 0700);
  return s;
}

void scripts_free(struct scripts *s)
{
  if (!s) return;
  while (s->list) {
    struct script *sc = s->list;
    s->list = sc->next;
    script_close(sc);
    free(sc->error);
    free(sc->name);
    free(sc);
  }
  free(s->dir);
  free(s);
}

const char *scripts_dir(struct scripts *s)
{
  return s->dir;
}

void scripts_reload(struct scripts *s)
{
  for (struct script *sc = s->list; sc; sc = sc->next) {
    script_close(sc);
    set_error(sc, NULL);
    sc->mtime = 0;
    sc->size = -1;
  }
}

cJSON *scripts_list(struct scripts *s)
{
  scan(s);
  cJSON *arr = cJSON_CreateArray();
  for (struct script *sc = s->list; sc; sc = sc->next) {
    script_load(s, sc);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", sc->name);
    cJSON_AddBoolToObject(o, "enabled", enabled(s, sc->name));
    cJSON *hooks = cJSON_AddArrayToObject(o, "hooks");
    if (sc->has_copy) cJSON_AddItemToArray(hooks, cJSON_CreateString("on_copy"));
    if (sc->has_paste) cJSON_AddItemToArray(hooks, cJSON_CreateString("on_paste"));
    if (sc->error) cJSON_AddStringToObject(o, "error", sc->error);
    else cJSON_AddNullToObject(o, "error");
    cJSON_AddItemToArray(arr, o);
  }
  return arr;
}

int scripts_enable(struct scripts *s, const char *name, bool on, const char **err)
{
  scan(s);
  if (!name || !find(s, name, false)) { *err = "no such script"; return -1; }
  sqlite3_stmt *st;
  if (sqlite3_prepare_v2(db_handle(s->app->db),
                         "INSERT INTO scripts(name, enabled) VALUES (?, ?)"
                         " ON CONFLICT(name) DO UPDATE SET enabled = excluded.enabled",
                         -1, &st, NULL) != SQLITE_OK) {
    *err = "database error";
    return -1;
  }
  sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
  sqlite3_bind_int(st, 2, on);
  int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(st);
  if (rc) *err = "database error";
  return rc;
}

cJSON *scripts_test(struct scripts *s, const char *name, const char *hook, int64_t clip_id, const char **err)
{
  scan(s);
  struct script *sc = name ? find(s, name, false) : NULL;
  if (!sc) { *err = "no such script"; return NULL; }
  bool copy = hook && !strcmp(hook, "on_copy");
  if (!copy && !(hook && !strcmp(hook, "on_paste"))) { *err = "hook is on_copy or on_paste"; return NULL; }
  struct clip_payload p;
  if (db_load_payload(s->app->db, clip_id, &p) < 0) { *err = "no such clip"; return NULL; }

  cJSON *res = cJSON_CreateObject();
  cJSON *log = cJSON_CreateArray();
  int64_t t0 = mono_ms();
  if (!script_load(s, sc)) {
    cJSON_AddStringToObject(res, "error", sc->error ? sc->error : "could not load");
  } else if (copy ? !sc->has_copy : !sc->has_paste) {
    cJSON_AddStringToObject(res, "error", copy ? "this script has no on_copy" : "this script has no on_paste");
  } else {
    struct fmt_in *fmts = payload_fmts(&p);
    cJSON *row = db_row(s->app->db, clip_id);
    const char *app = cJSON_GetStringValue(cJSON_GetObjectItem(row, "source_app"));
    sc->log = log;
    sc->log_lines = 0;
    size_t tlen;
    char *text = db_clip_text(s->app->db, clip_id, &tlen);
    if (copy) {
      struct copy_verdict v = { .sticky = -1, .locked = -1 };
      run_copy(sc, fmts, p.n_fmts, app, cJSON_GetStringValue(cJSON_GetObjectItem(row, "source_title")), &v);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "skip", v.skip);
      if (v.text) cJSON_AddStringToObject(r, "text", v.text);
      if (v.title) cJSON_AddStringToObject(r, "title", v.title);
      if (v.group) cJSON_AddStringToObject(r, "group", v.group);
      if (v.sticky >= 0) cJSON_AddBoolToObject(r, "sticky", v.sticky);
      if (v.locked >= 0) cJSON_AddBoolToObject(r, "locked", v.locked);
      cJSON_AddItemToObject(res, "result", r);
      copy_verdict_free(&v);
    } else {
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(row, "title"));
      char *out = run_paste(sc, fmts, p.n_fmts, text, text ? tlen : 0, t, "(test)");
      if (out) cJSON_AddStringToObject(res, "result", out);
      else cJSON_AddNullToObject(res, "result");
      free(out);
    }
    free(text);
    sc->log = NULL;
    if (sc->error) cJSON_AddStringToObject(res, "error", sc->error);
    cJSON_Delete(row);
    free(fmts);
  }
  cJSON_AddItemToObject(res, "log", log);
  cJSON_AddNumberToObject(res, "ms", (double)(mono_ms() - t0));
  clip_payload_free(&p);
  return res;
}

cJSON *scripts_install_examples(struct scripts *s, const char *examples_dir, const char **err)
{
  DIR *d = opendir(examples_dir);
  if (!d) { *err = "the bundled examples were not found"; return NULL; }
  cJSON *added = cJSON_CreateArray();
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!valid_name(de->d_name)) continue;
    char *dst = path_of(s, de->d_name);
    char *src = xasprintf("%s/%s", examples_dir, de->d_name);
    FILE *in = fopen(src, "rb");
    int out = in ? open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : -1; /* never overwrite */
    if (in && out >= 0) {
      char chunk[8192];
      size_t r;
      bool ok = true;
      while (ok && (r = fread(chunk, 1, sizeof chunk, in)) > 0) ok = write(out, chunk, r) == (ssize_t)r;
      if (ok) cJSON_AddItemToArray(added, cJSON_CreateString(de->d_name));
      else unlink(dst);
    }
    if (in) fclose(in);
    if (out >= 0) close(out);
    free(src);
    free(dst);
  }
  closedir(d);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddItemToObject(r, "added", added);
  cJSON_AddStringToObject(r, "dir", s->dir);
  return r;
}

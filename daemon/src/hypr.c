#include "hypr.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "app.h"
#include "cJSON.h"
#include "loop.h"
#include "util.h"

#define REQUEST_TIMEOUT_MS 500

struct hypr {
  struct app *app;
  char *req_path;
  char *evt_path;
  int evt_fd;
  struct loop_watch *watch;
  struct buf ebuf;
  struct loop_timer *reconnect_timer;
  int backoff_ms;
  char *active_class;
  char *active_title;
  hypr_reload_cb reload_cb;
  void *reload_ctx;
  hypr_window_cb window_cb;
  void *window_ctx;
};

static int connect_unix(const char *path, bool nonblock)
{
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof sa.sun_path) return -1;
  strcpy(sa.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  return fd;
}

char *hypr_request(struct hypr *h, const char *cmd, size_t *len)
{
  int fd = connect_unix(h->req_path, false);
  if (fd < 0) {
    log_warn("Hyprland request socket: %m");
    return NULL;
  }
  struct timeval tv = { .tv_sec = 0, .tv_usec = REQUEST_TIMEOUT_MS * 1000 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  size_t n = strlen(cmd);
  const char *p = cmd;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      log_warn("Hyprland request write: %m");
      close(fd);
      return NULL;
    }
    p += w;
    n -= (size_t)w;
  }
  /* Hyprland answers and closes; read to EOF. */
  struct buf out = { 0 };
  char chunk[8192];
  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) { buf_append(&out, chunk, (size_t)r); continue; }
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) log_warn("Hyprland request read: %m");
    break;
  }
  close(fd);
  if (!out.data) buf_append(&out, "", 0);
  return buf_steal(&out, len);
}

cJSON *hypr_query(struct hypr *h, const char *what)
{
  char *cmd = xasprintf("j/%s", what);
  char *reply = hypr_request(h, cmd, NULL);
  free(cmd);
  cJSON *j = reply ? cJSON_Parse(reply) : NULL;
  free(reply);
  return j;
}

int hypr_eval(struct hypr *h, const char *lua)
{
  char *cmd = xasprintf("eval %s", lua);
  char *reply = hypr_request(h, cmd, NULL);
  free(cmd);
  int rc = reply && !strncmp(reply, "ok", 2) ? 0 : -1;
  if (rc) log_err("Hyprland rejected Lua: %s", reply ? reply : "(no reply)");
  free(reply);
  return rc;
}

const char *hypr_active_class(struct hypr *h)
{
  return h->active_class;
}

const char *hypr_active_title(struct hypr *h)
{
  return h->active_title;
}

void hypr_on_reload(struct hypr *h, hypr_reload_cb cb, void *ctx)
{
  h->reload_cb = cb;
  h->reload_ctx = ctx;
}

void hypr_on_window_change(struct hypr *h, hypr_window_cb cb, void *ctx)
{
  h->window_cb = cb;
  h->window_ctx = ctx;
}

static void set_active(struct hypr *h, const char *cls, size_t cls_len, const char *title)
{
  free(h->active_class);
  free(h->active_title);
  h->active_class = cls_len ? xstrndup(cls, cls_len) : NULL;
  h->active_title = cls_len && title ? xstrdup(title) : NULL;
}

static void handle_event(struct hypr *h, char *line)
{
  char *sep = strstr(line, ">>");
  if (!sep) return;
  *sep = 0;
  const char *name = line, *data = sep + 2;
  if (!strcmp(name, "activewindow")) {
    /* "CLASS,TITLE": classes never contain a comma, titles may. */
    const char *comma = strchr(data, ',');
    if (comma) set_active(h, data, (size_t)(comma - data), comma + 1);
    else set_active(h, data, strlen(data), NULL);
    if (h->window_cb) h->window_cb(h->window_ctx);
  } else if (!strcmp(name, "closewindow")) {
    if (h->window_cb) h->window_cb(h->window_ctx);
  } else if (!strcmp(name, "configreloaded")) {
    if (h->reload_cb) h->reload_cb(h->reload_ctx);
  }
}

static void schedule_reconnect(struct hypr *h);

static void on_events(void *ctx, int fd, uint32_t events)
{
  struct hypr *h = ctx;
  char chunk[8192];
  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      buf_append(&h->ebuf, chunk, (size_t)r);
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && errno == EAGAIN) break;
    /* EOF or error: Hyprland went away (or is restarting). */
    log_warn("Hyprland event socket closed");
    loop_del_fd(h->app->loop, h->watch);
    h->watch = NULL;
    close(h->evt_fd);
    h->evt_fd = -1;
    buf_free(&h->ebuf);
    schedule_reconnect(h);
    return;
  }
  size_t start = 0;
  for (size_t i = 0; i < h->ebuf.len; i++) {
    if (h->ebuf.data[i] != '\n') continue;
    h->ebuf.data[i] = 0;
    handle_event(h, (char *)h->ebuf.data + start);
    start = i + 1;
  }
  if (start) {
    memmove(h->ebuf.data, h->ebuf.data + start, h->ebuf.len - start);
    h->ebuf.len -= start;
    h->ebuf.data[h->ebuf.len] = 0;
  }
  /* A runaway line (should never happen) must not grow without bound. */
  if (h->ebuf.len > 1 << 20) h->ebuf.len = 0;
}

static void refresh_active(struct hypr *h)
{
  cJSON *w = hypr_query(h, "activewindow");
  const char *cls = cJSON_GetStringValue(cJSON_GetObjectItem(w, "class"));
  const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(w, "title"));
  set_active(h, cls ? cls : "", cls ? strlen(cls) : 0, title);
  cJSON_Delete(w);
}

static bool open_events(struct hypr *h)
{
  h->evt_fd = connect_unix(h->evt_path, true);
  if (h->evt_fd < 0) return false;
  h->watch = loop_add_fd(h->app->loop, h->evt_fd, EPOLLIN, on_events, h);
  refresh_active(h);
  h->backoff_ms = 0;
  return true;
}

static void on_reconnect(void *ctx)
{
  struct hypr *h = ctx;
  h->reconnect_timer = NULL;
  if (open_events(h)) {
    log_info("Hyprland event socket reconnected");
    if (h->reload_cb) h->reload_cb(h->reload_ctx);
    return;
  }
  schedule_reconnect(h);
}

static void schedule_reconnect(struct hypr *h)
{
  h->backoff_ms = h->backoff_ms ? MIN(h->backoff_ms * 2, 10000) : 250;
  h->reconnect_timer = loop_timer(h->app->loop, h->backoff_ms, on_reconnect, h);
}

struct hypr *hypr_connect(struct app *app)
{
  const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  const char *rt = getenv("XDG_RUNTIME_DIR");
  if (!sig || !*sig || !rt) return NULL;
  struct hypr *h = xcalloc(1, sizeof *h);
  h->app = app;
  h->evt_fd = -1;
  h->req_path = xasprintf("%s/hypr/%s/.socket.sock", rt, sig);
  h->evt_path = xasprintf("%s/hypr/%s/.socket2.sock", rt, sig);
  if (!open_events(h)) {
    log_warn("Hyprland event socket unavailable (%m); retrying");
    schedule_reconnect(h);
  }
  return h;
}

void hypr_disconnect(struct hypr *h)
{
  if (!h) return;
  loop_timer_cancel(h->app->loop, h->reconnect_timer);
  if (h->watch) loop_del_fd(h->app->loop, h->watch);
  if (h->evt_fd >= 0) close(h->evt_fd);
  buf_free(&h->ebuf);
  free(h->active_class);
  free(h->active_title);
  free(h->req_path);
  free(h->evt_path);
  free(h);
}

void lua_quote(struct buf *b, const char *s)
{
  buf_append(b, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { buf_append(b, "\\", 1); buf_append(b, s, 1); }
    else if (c == '\n') buf_append_str(b, "\\n");
    else if (c < 0x20 || c == 0x7F) buf_printf(b, "\\%03u", c);
    else buf_append(b, s, 1);
  }
  buf_append(b, "\"", 1);
}

/* Case-insensitive shell glob → anchored Lua pattern over a lowercased class. */
static void glob_to_lua_pattern(struct buf *b, const char *glob)
{
  buf_append(b, "^", 1);
  for (const char *g = glob; *g; g++) {
    char c = (char)tolower((unsigned char)*g);
    if (c == '*') buf_append_str(b, ".*");
    else if (c == '?') buf_append(b, ".", 1);
    else if (strchr("^$()%.[]+-", c)) { buf_append(b, "%", 1); buf_append(b, &c, 1); }
    else buf_append(b, &c, 1);
  }
  buf_append(b, "$", 1);
}

/* The keystroke runs inside Hyprland so that the checks and the key happen
 * in the same place with no round trips: wait (in 15 ms steps, 600 ms at
 * most) until our popup's layer is unmapped and no modifier is physically
 * held, so the synthetic chord is not merged with a held Ctrl from Ctrl+1 or
 * a global hotkey, then press and release. The press/release split with a
 * 50 ms gap is Omarchy's workaround for stuck synthetic keys. */
static int send_chord(struct hypr *h, int delay_ms, const struct paste_key *ov, size_t n_ov, const char *mods,
                      const char *key, const char *term_mods, const char *term_key)
{
  struct buf b = { 0 };
  buf_append_str(&b,
                 "do\n"
                 "local tries = 0\n"
                 "local mods_held = { \"Control_L\", \"Control_R\", \"Shift_L\", \"Shift_R\","
                 " \"Alt_L\", \"Alt_R\", \"Super_L\", \"Super_R\" }\n"
                 "local overrides = {\n");
  for (size_t i = 0; i < n_ov; i++) {
    struct buf pat = { 0 };
    glob_to_lua_pattern(&pat, ov[i].pattern);
    buf_append_str(&b, "  { pat = ");
    lua_quote(&b, (char *)pat.data);
    buf_append_str(&b, ", mods = ");
    lua_quote(&b, ov[i].mods);
    buf_append_str(&b, ", key = ");
    lua_quote(&b, ov[i].key);
    buf_append_str(&b, " },\n");
    buf_free(&pat);
  }
  buf_append_str(&b, "}\nlocal default_mods, default_key = ");
  lua_quote(&b, mods);
  buf_append_str(&b, ", ");
  lua_quote(&b, key);
  buf_append_str(&b, "\nlocal term_mods, term_key = ");
  lua_quote(&b, term_mods);
  buf_append_str(&b, ", ");
  lua_quote(&b, term_key);
  buf_printf(&b,
             "\nlocal function busy()\n"
             "  if #hl.get_layers({ namespace = \"clipnet\" }) > 0 then return true end\n"
             "  for _, k in ipairs(mods_held) do if hl.is_key_down(k) then return true end end\n"
             "  return false\n"
             "end\n"
             "local function go()\n"
             "  tries = tries + 1\n"
             "  if busy() and tries < 40 then\n"
             "    hl.timer(go, { timeout = 15, type = \"oneshot\" })\n"
             "    return\n"
             "  end\n"
             "  local w = hl.get_active_window()\n"
             "  if not w then return end\n"
             "  local m, k = default_mods, default_key\n"
             "  local cls = string.lower(w.class or \"\")\n"
             "  local matched = false\n"
             "  for _, o in ipairs(overrides) do\n"
             "    if string.find(cls, o.pat) then m, k, matched = o.mods, o.key, true break end\n"
             "  end\n"
             "  if not matched then\n"
             "    for _, t in ipairs(w.tags or {}) do\n"
             "      if (t:gsub(\"%%*$\", \"\")) == \"terminal\" then m, k = term_mods, term_key end\n"
             "    end\n"
             "  end\n"
             "  hl.dispatch(hl.dsp.send_key_state({ mods = m, key = k, state = \"down\" }))\n"
             "  hl.timer(function()\n"
             "    hl.dispatch(hl.dsp.send_key_state({ mods = m, key = k, state = \"up\" }))\n"
             "  end, { timeout = 50, type = \"oneshot\" })\n"
             "end\n"
             "hl.timer(go, { timeout = %d, type = \"oneshot\" })\n"
             "end\n",
             delay_ms > 0 ? delay_ms : 1);
  int rc = hypr_eval(h, (char *)b.data);
  buf_free(&b);
  return rc;
}

int hypr_send_paste(struct hypr *h, int delay_ms, const struct paste_key *overrides, size_t n)
{
  return send_chord(h, delay_ms, overrides, n, "CTRL", "V", "SHIFT", "Insert");
}

int hypr_send_keys(struct hypr *h, int delay_ms, const char *mods, const char *key, const char *term_mods,
                   const char *term_key)
{
  return send_chord(h, delay_ms, NULL, 0, mods, key, term_mods, term_key);
}

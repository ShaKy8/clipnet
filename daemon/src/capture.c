#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "app.h"
#include "db.h"
#include "hypr.h"
#include "loop.h"
#include "mime.h"
#include "rules.h"
#include "script.h"
#include "serve.h"
#include "settings.h"
#include "util.h"
#include "wl.h"

/* Some apps set the selection several times in quick succession (a first
 * empty offer, then the real one). Wait this long for the dust to settle. */
#define DEBOUNCE_MS 30
#define FORMAT_TIMEOUT_MS 2000
#define TOTAL_TIMEOUT_MS 5000
/* After a hint that the clipboard owner may be gone, check this soon, and
 * once more later for apps that take a while to exit after their window
 * closes. */
#define KEEPALIVE_DELAY_MS 150
#define KEEPALIVE_RECHECK_MS 1200

struct got {
  const char *mime; /* canonical; points into plan/offer, valid while offer is */
  const char *offered;
  struct buf data;
};

struct capture {
  struct app *app;

  /* The selection being read. */
  struct wl_offer *offer;
  struct mime_plan plan;
  size_t next;
  int fd;
  struct loop_watch *watch;
  struct buf cur;
  size_t cur_cap;
  bool cur_over;
  int64_t deadline;
  struct got got[ARRAY_LEN(((struct mime_plan *)0)->reads)];
  size_t n_got;
  char *source_app, *source_title;

  struct loop_timer *debounce_timer, *format_timer, *keepalive_timer, *recheck_timer;
  bool probing;

  /* PRIMARY selection, read separately (text only). */
  struct wl_offer *primary_offer;
  char *primary_app;
  int primary_fd;
  struct loop_watch *primary_watch;
  struct buf primary_buf;
  struct loop_timer *primary_timer;

  int64_t current_clip;
  bool sensitive;

  capture_hook hook;
  void *hook_ctx;
  int64_t hook_deadline;
};

struct capture *capture_new(struct app *app)
{
  struct capture *c = xcalloc(1, sizeof *c);
  c->app = app;
  c->fd = -1;
  c->primary_fd = -1;
  return c;
}

static void stop_reading(struct capture *c)
{
  loop_timer_cancel(c->app->loop, c->format_timer);
  c->format_timer = NULL;
  if (c->watch) loop_del_fd(c->app->loop, c->watch);
  c->watch = NULL;
  if (c->fd >= 0) close(c->fd);
  c->fd = -1;
  buf_free(&c->cur);
}

/* Drop the in-flight capture, if any, and everything it gathered. */
static void abort_current(struct capture *c)
{
  loop_timer_cancel(c->app->loop, c->debounce_timer);
  c->debounce_timer = NULL;
  stop_reading(c);
  for (size_t i = 0; i < c->n_got; i++) buf_free(&c->got[i].data);
  c->n_got = 0;
  free(c->source_app);
  free(c->source_title);
  c->source_app = c->source_title = NULL;
  wl_offer_destroy(c->offer);
  c->offer = NULL;
}

static void primary_stop(struct capture *c);

void capture_free(struct capture *c)
{
  if (!c) return;
  abort_current(c);
  primary_stop(c);
  loop_timer_cancel(c->app->loop, c->keepalive_timer);
  loop_timer_cancel(c->app->loop, c->recheck_timer);
  free(c);
}

int64_t capture_current_clip(struct capture *c)
{
  return c->sensitive ? 0 : c->current_clip;
}

void capture_set_current_clip(struct capture *c, int64_t id)
{
  c->current_clip = id;
  c->sensitive = false;
}

void capture_expect(struct capture *c, int timeout_ms, capture_hook hook, void *ctx)
{
  c->hook = hook;
  c->hook_ctx = ctx;
  c->hook_deadline = mono_ms() + timeout_ms;
}

static void read_next(struct capture *c);

static bool text_from_latin1(const char *offered)
{
  return !strcmp(offered, "STRING") || !strcmp(offered, "TEXT");
}

static void finish(struct capture *c)
{
  struct app *app = c->app;
  struct fmt_in fmts[ARRAY_LEN(c->got)];
  size_t n = 0;
  bool any_bytes = false;
  char *converted = NULL;

  for (size_t i = 0; i < c->n_got; i++) {
    struct got *g = &c->got[i];
    struct fmt_in *f = &fmts[n++];
    *f = (struct fmt_in){ g->mime, g->data.data ? g->data.data : (const uint8_t *)"", g->data.len, NULL, 0 };
    if (!strcmp(g->mime, MIME_TEXT)) {
      /* Stored text is always UTF-8: it is offered back as charset=utf-8. */
      if (!utf8_valid(f->data, f->len) && text_from_latin1(g->offered)) {
        size_t len;
        converted = latin1_to_utf8(f->data, f->len, &len);
        f->data = (const uint8_t *)converted;
        f->len = len;
      }
      f->aliases = c->plan.text_aliases;
      f->n_aliases = c->plan.n_text_aliases;
    }
    if (f->len) any_bytes = true;
  }

  struct copy_verdict verdict = { .sticky = -1, .locked = -1 };
  if (any_bytes && app->scripts) {
    scripts_on_copy(app->scripts, fmts, n, c->source_app, c->source_title, &verdict);
    if (verdict.skip) {
      log_debug("a script chose not to store this copy");
      any_bytes = false;
    } else if (verdict.text) {
      /* New text: the other formats no longer match it, so it becomes a
       * plain-text clip (as an edit in the clip editor does). */
      fmts[0] = (struct fmt_in){ MIME_TEXT, (const uint8_t *)verdict.text, strlen(verdict.text), mime_text_default_aliases,
                                 mime_text_default_alias_count };
      n = 1;
    }
  }

  if (!any_bytes) {
    if (!verdict.skip) log_debug("selection had no content; not stored");
  } else {
    int64_t route = rules_route_group(app->db, c->source_app);
    if (verdict.group) {
      const char *gerr;
      int64_t g = db_group_ensure_path(app->db, verdict.group, &gerr);
      if (g > 0) route = g;
      else log_warn("script group \"%s\": %s", verdict.group, gerr);
    }
    bool keep_title = setting_bool(app->db, "store_source_title");
    struct clip_in in = {
      .fmts = fmts,
      .n_fmts = n,
      .source_app = c->source_app,
      .source_title = keep_title ? c->source_title : NULL,
      .group_id = route,
      .title = verdict.title,
    };
    int64_t id = 0;
    enum add_result r = db_add_clip(app->db, &in, &id);
    if (r == ADD_ERR) {
      log_err("could not store clip");
    } else {
      log_debug("captured clip %lld (%zu formats, %s)", (long long)id, n, r == ADD_NEW ? "new" : "duplicate");
      const char *e;
      if (verdict.sticky >= 0) db_clip_set_sticky(app->db, id, verdict.sticky ? STICKY_TOP : STICKY_OFF, &e);
      if (verdict.locked >= 0) db_clip_set_locked(app->db, id, verdict.locked, &e);
      if (r == ADD_DUP && verdict.title) db_clip_set_title(app->db, id, verdict.title, &e);
      /* The clip now differs from the selection only if a script rewrote
       * its text; the clipboard still holds the original. */
      c->current_clip = verdict.text ? 0 : id;
      app_emit_clip(app, r == ADD_NEW ? "clip.added" : "clip.updated", id);
      if (c->hook) {
        capture_hook hook = c->hook;
        void *ctx = c->hook_ctx;
        c->hook = NULL;
        if (mono_ms() <= c->hook_deadline) hook(ctx, id);
      }
    }
  }
  copy_verdict_free(&verdict);
  free(converted);
  abort_current(c);
}

static void end_format(struct capture *c, bool keep)
{
  const struct mime_read *r = &c->plan.reads[c->next];
  if (keep) {
    struct got *g = &c->got[c->n_got++];
    g->mime = r->canonical;
    g->offered = r->offered;
    g->data = c->cur;
    c->cur = (struct buf){ 0 };
  }
  stop_reading(c);
  c->next++;
  read_next(c);
}

static void on_format_timeout(void *ctx)
{
  struct capture *c = ctx;
  c->format_timer = NULL;
  log_warn("source did not finish sending %s in time; skipped", c->plan.reads[c->next].offered);
  end_format(c, false);
}

static void on_readable(void *ctx, int fd, uint32_t events)
{
  struct capture *c = ctx;
  uint8_t chunk[65536];
  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      if (c->cur.len + (size_t)r > c->cur_cap) {
        c->cur_over = true;
        log_warn("%s exceeds its size cap (%zu bytes); skipped", c->plan.reads[c->next].offered, c->cur_cap);
        end_format(c, false);
        return;
      }
      buf_append(&c->cur, chunk, (size_t)r);
      continue;
    }
    if (r == 0) {
      end_format(c, true);
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return;
    log_warn("reading %s: %m", c->plan.reads[c->next].offered);
    end_format(c, false);
    return;
  }
}

static size_t cap_for(struct capture *c, const char *canonical)
{
  const char *key = !strcmp(canonical, MIME_TEXT) ? "text_cap_bytes"
                    : mime_is_image(canonical)    ? "image_cap_bytes"
                                                  : "other_cap_bytes";
  return (size_t)setting_int(c->app->db, key);
}

static void read_next(struct capture *c)
{
  if (c->next >= c->plan.n_reads || mono_ms() >= c->deadline) {
    if (c->next < c->plan.n_reads) log_warn("capture ran out of time; kept %zu formats", c->n_got);
    finish(c);
    return;
  }
  const struct mime_read *r = &c->plan.reads[c->next];
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) {
    log_err("pipe: %m");
    finish(c);
    return;
  }
  /* Only our end is non-blocking; the source writes however it likes. */
  set_nonblock(p[0]);
  wl_offer_receive(c->app->wl, c->offer, r->offered, p[1]);
  close(p[1]);
  c->fd = p[0];
  c->cur_cap = cap_for(c, r->canonical);
  c->cur_over = false;
  c->watch = loop_add_fd(c->app->loop, c->fd, EPOLLIN, on_readable, c);
  int64_t left = c->deadline - mono_ms();
  c->format_timer = loop_timer(c->app->loop, MIN(left, FORMAT_TIMEOUT_MS), on_format_timeout, c);
}

static void on_debounced(void *ctx)
{
  struct capture *c = ctx;
  c->debounce_timer = NULL;
  c->deadline = mono_ms() + TOTAL_TIMEOUT_MS;
  c->next = 0;
  read_next(c);
}

static bool keepalive_wanted(struct capture *c)
{
  /* Only put back what we captured ourselves, and never after a password
   * manager cleared its secret on purpose. */
  return c->app->wl && c->current_clip && !c->sensitive && !c->offer && setting_bool(c->app->db, "keep_alive");
}

static void on_probe(void *ctx, bool has_selection)
{
  struct capture *c = ctx;
  c->probing = false;
  if (has_selection || !keepalive_wanted(c) || !db_exists(c->app->db, c->current_clip)) return;
  log_debug("clipboard is empty (its owner left); restoring clip %lld", (long long)c->current_clip);
  serve_clip(c->app->serve, c->current_clip, SERVE_ALL);
}

static void on_keepalive(void *ctx)
{
  struct capture *c = ctx;
  c->keepalive_timer = NULL;
  if (!keepalive_wanted(c) || c->probing) return;
  /* Nothing to check while we are the owner ourselves. */
  if (serve_current(c->app->serve) == c->current_clip && serve_current(c->app->serve)) return;
  c->probing = true;
  wl_probe_selection(c->app->wl, on_probe, c);
}

static void on_recheck(void *ctx)
{
  struct capture *c = ctx;
  c->recheck_timer = NULL;
  on_keepalive(c);
}

void capture_owner_may_have_left(struct capture *c)
{
  if (!keepalive_wanted(c)) return;
  loop_timer_cancel(c->app->loop, c->keepalive_timer);
  loop_timer_cancel(c->app->loop, c->recheck_timer);
  c->keepalive_timer = loop_timer(c->app->loop, KEEPALIVE_DELAY_MS, on_keepalive, c);
  c->recheck_timer = loop_timer(c->app->loop, KEEPALIVE_RECHECK_MS, on_recheck, c);
}

void capture_on_selection(struct capture *c, struct wl_offer *offer)
{
  struct app *app = c->app;
  loop_timer_cancel(app->loop, c->keepalive_timer);
  c->keepalive_timer = NULL;
  abort_current(c);

  if (!offer) {
    capture_owner_may_have_left(c);
    return;
  }

  mime_plan_build(&c->plan, (const char *const *)offer->mimes, offer->n_mimes);
  if (c->plan.is_ours) {
    /* Our own paste coming back: the clip is already stored and current. */
    wl_offer_destroy(offer);
    return;
  }
  c->current_clip = 0;
  if (c->plan.is_sensitive) {
    c->sensitive = true;
    log_debug("selection marked sensitive by its source; not stored");
    wl_offer_destroy(offer);
    return;
  }
  c->sensitive = false;
  if (app_paused(app)) {
    wl_offer_destroy(offer);
    return;
  }
  const char *app_class = app->hypr ? hypr_active_class(app->hypr) : NULL;
  if (rules_excluded(app->db, app_class)) {
    log_debug("selection from excluded app %s; not stored", app_class);
    wl_offer_destroy(offer);
    return;
  }
  rules_filter_plan(app->db, app_class, &c->plan);
  if (!c->plan.n_reads) {
    wl_offer_destroy(offer);
    return;
  }
  c->offer = offer;
  c->source_app = app_class ? xstrdup(app_class) : NULL;
  const char *title = app->hypr ? hypr_active_title(app->hypr) : NULL;
  c->source_title = title ? xstrdup(title) : NULL;
  c->debounce_timer = loop_timer(app->loop, DEBOUNCE_MS, on_debounced, c);
}

/* ---- PRIMARY (select-to-copy) ------------------------------------------
 *
 * Optional and off by default: every text selection would otherwise land in
 * the history. Text only, and only once the selection has held still for a
 * moment, since it changes continuously while the mouse drags. */

#define PRIMARY_SETTLE_MS 700
#define PRIMARY_CAP (1024 * 1024)

static void primary_stop(struct capture *c)
{
  loop_timer_cancel(c->app->loop, c->primary_timer);
  c->primary_timer = NULL;
  if (c->primary_watch) loop_del_fd(c->app->loop, c->primary_watch);
  c->primary_watch = NULL;
  if (c->primary_fd >= 0) close(c->primary_fd);
  c->primary_fd = -1;
  buf_free(&c->primary_buf);
  wl_offer_destroy(c->primary_offer);
  c->primary_offer = NULL;
  free(c->primary_app);
  c->primary_app = NULL;
}

static void primary_store(struct capture *c)
{
  struct app *app = c->app;
  const char *t = c->primary_buf.data ? (const char *)c->primary_buf.data : "";
  size_t len = c->primary_buf.len;
  bool blank = true;
  for (size_t i = 0; i < len && blank; i++) blank = t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r';
  struct copy_verdict v = { .sticky = -1, .locked = -1 };
  struct fmt_in f = { MIME_TEXT, (const uint8_t *)t, len, mime_text_default_aliases, mime_text_default_alias_count };
  if (!blank && utf8_valid((const uint8_t *)t, len) && app->scripts) {
    scripts_on_copy(app->scripts, &f, 1, c->primary_app, NULL, &v);
    if (v.text) { f.data = (const uint8_t *)v.text; f.len = strlen(v.text); }
  }
  if (!blank && !v.skip && utf8_valid((const uint8_t *)t, len)) {
    int64_t group = rules_route_group(app->db, c->primary_app);
    const char *gerr;
    if (v.group) { int64_t g = db_group_ensure_path(app->db, v.group, &gerr); if (g > 0) group = g; }
    struct clip_in in = { .fmts = &f, .n_fmts = 1, .source_app = c->primary_app, .group_id = group, .title = v.title };
    int64_t id;
    enum add_result r = db_add_clip(app->db, &in, &id);
    if (r != ADD_ERR) {
      const char *e;
      if (v.sticky >= 0) db_clip_set_sticky(app->db, id, v.sticky ? STICKY_TOP : STICKY_OFF, &e);
      if (v.locked >= 0) db_clip_set_locked(app->db, id, v.locked, &e);
      app_emit_clip(app, r == ADD_NEW ? "clip.added" : "clip.updated", id);
    }
  }
  copy_verdict_free(&v);
  primary_stop(c);
}

static void on_primary_readable(void *ctx, int fd, uint32_t events)
{
  struct capture *c = ctx;
  char chunk[65536];
  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      if (c->primary_buf.len + (size_t)r > PRIMARY_CAP) { primary_stop(c); return; }
      buf_append(&c->primary_buf, chunk, (size_t)r);
      continue;
    }
    if (r == 0) { primary_store(c); return; }
    if (errno == EINTR) continue;
    if (errno != EAGAIN) primary_stop(c);
    return;
  }
}

static void on_primary_timeout(void *ctx)
{
  struct capture *c = ctx;
  c->primary_timer = NULL;
  primary_stop(c);
}

static void on_primary_settled(void *ctx)
{
  struct capture *c = ctx;
  c->primary_timer = NULL;
  struct mime_plan plan;
  mime_plan_build(&plan, (const char *const *)c->primary_offer->mimes, c->primary_offer->n_mimes);
  if (!plan.n_reads || strcmp(plan.reads[0].canonical, MIME_TEXT)) { primary_stop(c); return; }
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0) { primary_stop(c); return; }
  set_nonblock(p[0]);
  wl_offer_receive(c->app->wl, c->primary_offer, plan.reads[0].offered, p[1]);
  close(p[1]);
  c->primary_fd = p[0];
  c->primary_watch = loop_add_fd(c->app->loop, c->primary_fd, EPOLLIN, on_primary_readable, c);
  c->primary_timer = loop_timer(c->app->loop, FORMAT_TIMEOUT_MS, on_primary_timeout, c);
}

void capture_on_primary(struct capture *c, struct wl_offer *offer)
{
  struct app *app = c->app;
  primary_stop(c);
  if (!offer) return;
  if (!setting_bool(app->db, "capture_primary") || app_paused(app)) { wl_offer_destroy(offer); return; }
  struct mime_plan plan;
  mime_plan_build(&plan, (const char *const *)offer->mimes, offer->n_mimes);
  const char *app_class = app->hypr ? hypr_active_class(app->hypr) : NULL;
  if (plan.is_ours || plan.is_sensitive || rules_excluded(app->db, app_class)) { wl_offer_destroy(offer); return; }
  c->primary_offer = offer;
  c->primary_app = app_class ? xstrdup(app_class) : NULL;
  c->primary_timer = loop_timer(app->loop, PRIMARY_SETTLE_MS, on_primary_settled, c);
}

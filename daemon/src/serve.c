#include "serve.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "app.h"
#include "capture.h"
#include "db.h"
#include "ext-data-control-v1.h"
#include "loop.h"
#include "mime.h"
#include "util.h"
#include "wl.h"

#define CHUNK (64 * 1024)
#define WRITER_TIMEOUT_MS 10000

/* Payload bytes shared by a source and every transfer reading from it: a
 * transfer may outlive its source when a new selection replaces it. */
struct payload {
  int refs;
  struct clip_payload cp;
};

struct source {
  struct serve *serve;
  struct ext_data_control_source_v1 *proxy;
  struct payload *pl;
  char **names;   /* offered mime names */
  size_t *fmt_of; /* names[i] → cp.fmts index */
  size_t n_names;
  int64_t clip_id;
};

struct writer {
  struct serve *serve;
  struct payload *pl;
  const uint8_t *p;
  size_t left;
  int fd;
  struct loop_watch *watch;
  struct loop_timer *timer;
  struct writer *next;
};

struct serve {
  struct app *app;
  struct source *current;
  struct writer *writers;
};

static struct payload *payload_ref(struct payload *pl)
{
  pl->refs++;
  return pl;
}

static void payload_unref(struct payload *pl)
{
  if (!pl || --pl->refs > 0) return;
  clip_payload_free(&pl->cp);
  free(pl);
}

static void writer_free(struct writer *w)
{
  struct serve *s = w->serve;
  for (struct writer **pp = &s->writers; *pp; pp = &(*pp)->next) {
    if (*pp == w) { *pp = w->next; break; }
  }
  loop_timer_cancel(s->app->loop, w->timer);
  if (w->watch) loop_del_fd(s->app->loop, w->watch);
  close(w->fd);
  payload_unref(w->pl);
  free(w);
}

/* Write as much as the pipe takes. Returns true when the writer is done
 * (finished or failed) and has been freed. */
static bool writer_pump(struct writer *w)
{
  while (w->left) {
    ssize_t r = write(w->fd, w->p, MIN(w->left, (size_t)CHUNK));
    if (r > 0) {
      w->p += r;
      w->left -= (size_t)r;
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && errno == EAGAIN) return false;
    /* EPIPE: the reader went away (it asked, then lost interest). Normal. */
    if (errno != EPIPE) log_warn("serving clipboard data: %m");
    writer_free(w);
    return true;
  }
  writer_free(w);
  return true;
}

static void on_writable(void *ctx, int fd, uint32_t events)
{
  struct writer *w = ctx;
  if (events & (EPOLLERR | EPOLLHUP)) {
    writer_free(w);
    return;
  }
  writer_pump(w);
}

static void on_writer_timeout(void *ctx)
{
  struct writer *w = ctx;
  w->timer = NULL;
  log_warn("a clipboard reader stalled; dropped after %d ms", WRITER_TIMEOUT_MS);
  writer_free(w);
}

static void start_writer(struct serve *s, struct payload *pl, const uint8_t *p, size_t n, int fd)
{
  struct writer *w = xcalloc(1, sizeof *w);
  w->serve = s;
  w->pl = pl ? payload_ref(pl) : NULL;
  w->p = p;
  w->left = n;
  w->fd = fd;
  w->next = s->writers;
  s->writers = w;
  set_nonblock(fd);
  if (writer_pump(w)) return; /* small payloads finish right here */
  w->watch = loop_add_fd(s->app->loop, fd, EPOLLOUT, on_writable, w);
  w->timer = loop_timer(s->app->loop, WRITER_TIMEOUT_MS, on_writer_timeout, w);
}

static void source_free(struct source *src)
{
  if (!src) return;
  if (src->serve->current == src) src->serve->current = NULL;
  wl_source_destroy(src->proxy);
  for (size_t i = 0; i < src->n_names; i++) free(src->names[i]);
  free(src->names);
  free(src->fmt_of);
  payload_unref(src->pl);
  free(src);
}

static void on_send(void *data, struct ext_data_control_source_v1 *proxy, const char *mime, int fd)
{
  struct source *src = data;
  if (!strcmp(mime, MIME_MARKER)) {
    const char *uuid = src->pl->cp.uuid;
    start_writer(src->serve, src->pl, (const uint8_t *)uuid, strlen(uuid), fd);
    return;
  }
  for (size_t i = 0; i < src->n_names; i++) {
    if (!strcmp(src->names[i], mime)) {
      const struct fmt_out *f = &src->pl->cp.fmts[src->fmt_of[i]];
      start_writer(src->serve, src->pl, f->data, f->len, fd);
      return;
    }
  }
  /* A type we never offered. Close so the reader sees EOF, not a hang. */
  close(fd);
}

static void on_cancelled(void *data, struct ext_data_control_source_v1 *proxy)
{
  source_free(data);
}

static const struct ext_data_control_source_v1_listener source_listener = {
  .send = on_send,
  .cancelled = on_cancelled,
};

static void add_name(struct source *src, const char *name, size_t fmt)
{
  for (size_t i = 0; i < src->n_names; i++)
    if (!strcmp(src->names[i], name)) return;
  src->names = xrealloc(src->names, (src->n_names + 1) * sizeof *src->names);
  src->fmt_of = xrealloc(src->fmt_of, (src->n_names + 1) * sizeof *src->fmt_of);
  src->names[src->n_names] = xstrdup(name);
  src->fmt_of[src->n_names] = fmt;
  src->n_names++;
}

/* Take ownership of pl (one reference) and put it on the clipboard. */
static int own(struct serve *s, struct payload *pl, int64_t clip_id)
{
  if (!s->app->wl) {
    payload_unref(pl);
    return -1;
  }
  struct source *src = xcalloc(1, sizeof *src);
  src->serve = s;
  src->pl = pl;
  src->clip_id = clip_id;
  for (size_t i = 0; i < pl->cp.n_fmts; i++) {
    const struct fmt_out *f = &pl->cp.fmts[i];
    if (!strcmp(f->mime, MIME_TEXT)) {
      /* The canonical name first, then exactly what the source used, then
       * the usual set, so X11 and Wayland readers both find a name they know. */
      add_name(src, MIME_TEXT, i);
      for (size_t k = 0; k < f->n_aliases; k++) add_name(src, f->aliases[k], i);
      for (size_t k = 0; k < mime_text_default_alias_count; k++) add_name(src, mime_text_default_aliases[k], i);
    } else {
      add_name(src, f->mime, i);
    }
  }
  src->proxy = wl_source_create(s->app->wl, &source_listener, src);
  for (size_t i = 0; i < src->n_names; i++) wl_source_offer(src->proxy, src->names[i]);
  wl_source_offer(src->proxy, MIME_MARKER);
  /* The previous source gets `cancelled` from the compositor and frees
   * itself then; transfers already running from it keep their payload. */
  wl_set_selection(s->app->wl, src->proxy);
  s->current = src;
  capture_set_current_clip(s->app->capture, clip_id);
  return 0;
}

static struct payload *text_payload(const char *text, size_t len, const char *uuid)
{
  struct payload *pl = xcalloc(1, sizeof *pl);
  pl->refs = 1;
  pl->cp.fmts = xcalloc(1, sizeof *pl->cp.fmts);
  pl->cp.n_fmts = 1;
  struct fmt_out *f = &pl->cp.fmts[0];
  f->mime = xstrdup(MIME_TEXT);
  uint8_t *copy = xmalloc(len + 1);
  memcpy(copy, text, len);
  copy[len] = 0;
  f->data = copy;
  f->len = len;
  snprintf(pl->cp.uuid, sizeof pl->cp.uuid, "%s", uuid ? uuid : "text");
  return pl;
}

int serve_clip(struct serve *s, int64_t id, enum serve_mode mode)
{
  if (mode == SERVE_PLAIN) {
    size_t len;
    char *text = db_clip_text(s->app->db, id, &len);
    if (!text) return -1;
    char uuid[37] = "text";
    db_clip_uuid(s->app->db, id, uuid);
    int rc = own(s, text_payload(text, len, uuid), id);
    free(text);
    return rc;
  }
  struct payload *pl = xcalloc(1, sizeof *pl);
  pl->refs = 1;
  if (db_load_payload(s->app->db, id, &pl->cp) < 0) {
    free(pl);
    return -1;
  }
  return own(s, pl, id);
}

int serve_text(struct serve *s, const char *text, size_t len, int64_t clip_id)
{
  return own(s, text_payload(text, len, NULL), clip_id);
}

int64_t serve_current(struct serve *s)
{
  return s->current ? s->current->clip_id : 0;
}

int serve_pending_writes(struct serve *s)
{
  int n = 0;
  for (struct writer *w = s->writers; w; w = w->next) n++;
  return n;
}

struct serve *serve_new(struct app *app)
{
  struct serve *s = xcalloc(1, sizeof *s);
  s->app = app;
  return s;
}

void serve_free(struct serve *s)
{
  if (!s) return;
  while (s->writers) writer_free(s->writers);
  source_free(s->current);
  free(s);
}

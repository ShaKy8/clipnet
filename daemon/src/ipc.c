#include "ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "app.h"
#include "cJSON.h"
#include "loop.h"
#include "proto.h"
#include "util.h"

#define MAX_LINE (16 * 1024 * 1024)
/* A client that stops reading is cut off rather than buffered forever. */
#define MAX_PENDING_OUT (64 * 1024 * 1024)

struct ipc_client {
  struct ipc *ipc;
  int fd;
  struct loop_watch *watch;
  struct buf in, out;
  size_t out_off;
  bool subscribed;
  bool closing;
  struct ipc_client *next;
};

struct ipc {
  struct app *app;
  int fd;
  char *path;
  struct loop_watch *watch;
  struct ipc_client *clients;
  struct loop_timer *reap_timer;
};

static void client_free(struct ipc_client *c)
{
  struct ipc *ipc = c->ipc;
  for (struct ipc_client **pp = &ipc->clients; *pp; pp = &(*pp)->next) {
    if (*pp == c) { *pp = c->next; break; }
  }
  loop_del_fd(ipc->app->loop, c->watch);
  close(c->fd);
  buf_free(&c->in);
  buf_free(&c->out);
  free(c);
}

static void update_events(struct ipc_client *c)
{
  uint32_t ev = EPOLLIN | (c->out.len > c->out_off ? EPOLLOUT : 0);
  loop_mod_fd(c->ipc->app->loop, c->watch, ev);
}

static void flush_out(struct ipc_client *c)
{
  while (c->out_off < c->out.len) {
    ssize_t w = send(c->fd, c->out.data + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
    if (w > 0) { c->out_off += (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && errno == EAGAIN) break;
    c->closing = true;
    return;
  }
  if (c->out_off == c->out.len) {
    c->out.len = 0;
    c->out_off = 0;
  } else if (c->out_off > (1 << 20)) {
    memmove(c->out.data, c->out.data + c->out_off, c->out.len - c->out_off);
    c->out.len -= c->out_off;
    c->out_off = 0;
  }
  update_events(c);
}

static void send_json(struct ipc_client *c, cJSON *msg)
{
  char *s = cJSON_PrintUnformatted(msg);
  cJSON_Delete(msg);
  if (!s) return;
  if (c->out.len - c->out_off + strlen(s) > MAX_PENDING_OUT) {
    log_warn("IPC client not reading; disconnecting it");
    c->closing = true;
    free(s);
    return;
  }
  buf_append_str(&c->out, s);
  buf_append(&c->out, "\n", 1);
  free(s);
  flush_out(c);
}

/* Clients that failed during a write are freed from a fresh loop iteration,
 * never from inside a callback that may still be using them. */
static void on_reap(void *ctx)
{
  struct ipc *ipc = ctx;
  ipc->reap_timer = NULL;
  for (struct ipc_client *c = ipc->clients, *next; c; c = next) {
    next = c->next;
    if (c->closing) client_free(c);
  }
}

static void schedule_reap(struct ipc *ipc)
{
  if (!ipc->reap_timer) ipc->reap_timer = loop_timer(ipc->app->loop, 0, on_reap, ipc);
}

void ipc_reply(struct ipc_client *c, const cJSON *id, cJSON *result)
{
  cJSON *m = cJSON_CreateObject();
  if (id) cJSON_AddItemToObject(m, "rid", cJSON_Duplicate(id, 1));
  cJSON_AddTrueToObject(m, "ok");
  cJSON_AddItemToObject(m, "result", result ? result : cJSON_CreateNull());
  send_json(c, m);
}

void ipc_error(struct ipc_client *c, const cJSON *id, const char *code, const char *message)
{
  cJSON *m = cJSON_CreateObject();
  if (id) cJSON_AddItemToObject(m, "rid", cJSON_Duplicate(id, 1));
  cJSON_AddFalseToObject(m, "ok");
  cJSON *e = cJSON_AddObjectToObject(m, "error");
  cJSON_AddStringToObject(e, "code", code);
  cJSON_AddStringToObject(e, "message", message);
  send_json(c, m);
}

void ipc_subscribe(struct ipc_client *c, bool on)
{
  c->subscribed = on;
}

void ipc_broadcast(struct ipc *ipc, const char *event, cJSON *data)
{
  if (!ipc) { cJSON_Delete(data); return; }
  char *s = NULL;
  for (struct ipc_client *c = ipc->clients; c; c = c->next) {
    if (!c->subscribed || c->closing) continue;
    if (!s) {
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "event", event);
      cJSON_AddItemToObject(m, "data", data ? data : cJSON_CreateObject());
      data = NULL;
      s = cJSON_PrintUnformatted(m);
      cJSON_Delete(m);
    }
    if (c->out.len - c->out_off + strlen(s) > MAX_PENDING_OUT) {
      c->closing = true;
      continue;
    }
    buf_append_str(&c->out, s);
    buf_append(&c->out, "\n", 1);
    flush_out(c);
  }
  free(s);
  cJSON_Delete(data);
  schedule_reap(ipc);
}

static void handle_line(struct ipc_client *c, char *line, size_t len)
{
  if (!len) return;
  cJSON *req = cJSON_ParseWithLength(line, len);
  if (!cJSON_IsObject(req)) {
    ipc_error(c, NULL, "bad_request", "not a JSON object");
    cJSON_Delete(req);
    return;
  }
  proto_handle(c->ipc->app, c, req);
  cJSON_Delete(req);
}

static void on_client(void *ctx, int fd, uint32_t events)
{
  struct ipc_client *c = ctx;
  if (events & EPOLLOUT) flush_out(c);
  if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
    char chunk[65536];
    for (;;) {
      ssize_t r = recv(fd, chunk, sizeof chunk, 0);
      if (r > 0) {
        buf_append(&c->in, chunk, (size_t)r);
        if (c->in.len > MAX_LINE) {
          ipc_error(c, NULL, "too_large", "request line over 16 MiB");
          c->closing = true;
          break;
        }
        continue;
      }
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 && errno == EAGAIN) break;
      c->closing = true; /* EOF or error */
      break;
    }
    /* Handle every complete line; requests are answered in order. */
    size_t start = 0;
    for (size_t i = 0; i < c->in.len && !c->closing; i++) {
      if (c->in.data[i] != '\n') continue;
      handle_line(c, (char *)c->in.data + start, i - start);
      start = i + 1;
    }
    if (start) {
      memmove(c->in.data, c->in.data + start, c->in.len - start);
      c->in.len -= start;
    }
  }
  /* Let replies drain before closing a client that hung up after asking. */
  if (c->closing) {
    flush_out(c);
    client_free(c);
  }
}

static void on_accept(void *ctx, int fd, uint32_t events)
{
  struct ipc *ipc = ctx;
  for (;;) {
    int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno != EAGAIN && errno != EINTR) log_warn("accept: %m");
      if (errno == EINTR) continue;
      return;
    }
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != getuid()) {
      log_warn("refused IPC connection from another user");
      close(cfd);
      continue;
    }
    struct ipc_client *c = xcalloc(1, sizeof *c);
    c->ipc = ipc;
    c->fd = cfd;
    c->watch = loop_add_fd(ipc->app->loop, cfd, EPOLLIN, on_client, c);
    c->next = ipc->clients;
    ipc->clients = c;
  }
}

struct ipc *ipc_listen(struct app *app, const char *path)
{
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof sa.sun_path) {
    log_err("socket path too long: %s", path);
    return NULL;
  }
  strcpy(sa.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return NULL;
  /* The single-instance lock is held by main, so a leftover file is stale. */
  unlink(path);
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || chmod(path, 0600) < 0 || listen(fd, 16) < 0) {
    log_err("listen on %s: %m", path);
    close(fd);
    return NULL;
  }
  struct ipc *ipc = xcalloc(1, sizeof *ipc);
  ipc->app = app;
  ipc->fd = fd;
  ipc->path = xstrdup(path);
  ipc->watch = loop_add_fd(app->loop, fd, EPOLLIN, on_accept, ipc);
  return ipc;
}

void ipc_close(struct ipc *ipc)
{
  if (!ipc) return;
  loop_timer_cancel(ipc->app->loop, ipc->reap_timer);
  while (ipc->clients) client_free(ipc->clients);
  loop_del_fd(ipc->app->loop, ipc->watch);
  close(ipc->fd);
  unlink(ipc->path);
  free(ipc->path);
  free(ipc);
}

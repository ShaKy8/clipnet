#include "loop.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "util.h"

struct loop_watch {
  int fd;
  loop_fd_cb cb;
  void *ctx;
  bool dead;
  struct loop_watch *next_dead;
};

struct loop_timer {
  int64_t due;
  loop_timer_cb cb;
  void *ctx;
  struct loop_timer *next;
};

struct loop {
  int ep;
  bool quit;
  struct loop_timer *timers;   /* sorted by due */
  struct loop_watch *graveyard; /* deleted during dispatch, freed after */
  loop_prepare_cb prepare;
  void *prepare_ctx;
};

struct loop *loop_new(void)
{
  struct loop *l = xcalloc(1, sizeof *l);
  l->ep = epoll_create1(EPOLL_CLOEXEC);
  if (l->ep < 0) { free(l); return NULL; }
  return l;
}

void loop_free(struct loop *l)
{
  if (!l) return;
  while (l->timers) {
    struct loop_timer *t = l->timers;
    l->timers = t->next;
    free(t);
  }
  while (l->graveyard) {
    struct loop_watch *w = l->graveyard;
    l->graveyard = w->next_dead;
    free(w);
  }
  close(l->ep);
  free(l);
}

struct loop_watch *loop_add_fd(struct loop *l, int fd, uint32_t events, loop_fd_cb cb, void *ctx)
{
  struct loop_watch *w = xcalloc(1, sizeof *w);
  w->fd = fd;
  w->cb = cb;
  w->ctx = ctx;
  struct epoll_event ev = { .events = events, .data.ptr = w };
  if (epoll_ctl(l->ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
    log_err("epoll add fd %d: %m", fd);
    free(w);
    return NULL;
  }
  return w;
}

void loop_mod_fd(struct loop *l, struct loop_watch *w, uint32_t events)
{
  struct epoll_event ev = { .events = events, .data.ptr = w };
  if (epoll_ctl(l->ep, EPOLL_CTL_MOD, w->fd, &ev) < 0) log_err("epoll mod fd %d: %m", w->fd);
}

void loop_del_fd(struct loop *l, struct loop_watch *w)
{
  if (!w || w->dead) return;
  epoll_ctl(l->ep, EPOLL_CTL_DEL, w->fd, NULL);
  /* An event for w may still sit in the current epoll batch; keep the memory
   * alive until the batch is done and mark it so it is skipped. */
  w->dead = true;
  w->next_dead = l->graveyard;
  l->graveyard = w;
}

struct loop_timer *loop_timer(struct loop *l, int64_t delay_ms, loop_timer_cb cb, void *ctx)
{
  struct loop_timer *t = xcalloc(1, sizeof *t);
  t->due = mono_ms() + (delay_ms > 0 ? delay_ms : 0);
  t->cb = cb;
  t->ctx = ctx;
  struct loop_timer **pp = &l->timers;
  while (*pp && (*pp)->due <= t->due) pp = &(*pp)->next;
  t->next = *pp;
  *pp = t;
  return t;
}

void loop_timer_cancel(struct loop *l, struct loop_timer *t)
{
  if (!t) return;
  for (struct loop_timer **pp = &l->timers; *pp; pp = &(*pp)->next) {
    if (*pp == t) {
      *pp = t->next;
      free(t);
      return;
    }
  }
  /* Not in the list: it already fired (or is firing), nothing to do. */
}

void loop_set_prepare(struct loop *l, loop_prepare_cb cb, void *ctx)
{
  l->prepare = cb;
  l->prepare_ctx = ctx;
}

void loop_quit(struct loop *l)
{
  l->quit = true;
}

static void run_timers(struct loop *l)
{
  int64_t now = mono_ms();
  /* Pop each due timer before calling it so callbacks may add or cancel
   * timers (including re-arming themselves) without corrupting the list. */
  while (l->timers && l->timers->due <= now && !l->quit) {
    struct loop_timer *t = l->timers;
    l->timers = t->next;
    loop_timer_cb cb = t->cb;
    void *ctx = t->ctx;
    free(t);
    cb(ctx);
  }
}

int loop_run(struct loop *l)
{
  struct epoll_event evs[32];
  while (!l->quit) {
    if (l->prepare) l->prepare(l->prepare_ctx);
    int timeout = -1;
    if (l->timers) {
      int64_t d = l->timers->due - mono_ms();
      timeout = d < 0 ? 0 : d > 1 << 30 ? 1 << 30 : (int)d;
    }
    int n = epoll_wait(l->ep, evs, ARRAY_LEN(evs), timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      log_err("epoll_wait: %m");
      return -1;
    }
    for (int i = 0; i < n && !l->quit; i++) {
      struct loop_watch *w = evs[i].data.ptr;
      if (!w->dead) w->cb(w->ctx, w->fd, evs[i].events);
    }
    while (l->graveyard) {
      struct loop_watch *w = l->graveyard;
      l->graveyard = w->next_dead;
      free(w);
    }
    run_timers(l);
  }
  return 0;
}

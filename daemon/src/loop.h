/*
 * Single-threaded event loop: epoll for fds, a sorted list for timers.
 *
 * With no timer pending, loop_run blocks in epoll_wait without a timeout, so
 * an idle daemon costs no wakeups at all.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct loop;
struct loop_watch;
struct loop_timer;

typedef void (*loop_fd_cb)(void *ctx, int fd, uint32_t events);
typedef void (*loop_timer_cb)(void *ctx);
/* Called before every wait; used to flush the Wayland display. */
typedef void (*loop_prepare_cb)(void *ctx);

struct loop *loop_new(void);
void loop_free(struct loop *l);

struct loop_watch *loop_add_fd(struct loop *l, int fd, uint32_t events, loop_fd_cb cb, void *ctx);
void loop_mod_fd(struct loop *l, struct loop_watch *w, uint32_t events);
/* Safe to call from inside the watch's own callback. Does not close fd. */
void loop_del_fd(struct loop *l, struct loop_watch *w);

/* One-shot timer. The handle is valid until the timer fires or is cancelled;
 * a callback must clear any stored copy of its own handle first thing, since
 * the memory is gone and the address may be reused by the next timer. */
struct loop_timer *loop_timer(struct loop *l, int64_t delay_ms, loop_timer_cb cb, void *ctx);
/* Cancel a pending timer; NULL is fine. */
void loop_timer_cancel(struct loop *l, struct loop_timer *t);

void loop_set_prepare(struct loop *l, loop_prepare_cb cb, void *ctx);
int loop_run(struct loop *l);      /* until loop_quit */
void loop_quit(struct loop *l);

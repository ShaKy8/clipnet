#include "wl.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "capture.h"
#include "ext-data-control-v1.h"
#include "hyprland-global-shortcuts-v1.h"
#include "loop.h"
#include "util.h"

#define MAX_MIMES 128

struct wl {
  struct app *app;
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct ext_data_control_manager_v1 *manager;
  struct ext_data_control_device_v1 *device;
  struct hyprland_global_shortcuts_manager_v1 *shortcuts;
  struct wl_shortcut *shortcut_list;
  struct loop_watch *watch;
  bool want_write;
};

struct wl_shortcut {
  struct hyprland_global_shortcut_v1 *proxy;
  char *id;
  wl_shortcut_cb cb;
  void *ctx;
  struct wl_shortcut *next;
};

static void shortcut_pressed(void *data, struct hyprland_global_shortcut_v1 *s, uint32_t hi, uint32_t lo, uint32_t ns)
{
  struct wl_shortcut *sc = data;
  sc->cb(sc->ctx, sc->id);
}

static void shortcut_released(void *data, struct hyprland_global_shortcut_v1 *s, uint32_t hi, uint32_t lo, uint32_t ns) {}

static const struct hyprland_global_shortcut_v1_listener shortcut_listener = {
  .pressed = shortcut_pressed,
  .released = shortcut_released,
};

bool wl_has_shortcuts(struct wl *wl)
{
  return wl && wl->shortcuts;
}

struct wl_shortcut *wl_shortcut_register(struct wl *wl, const char *app_id, const char *id, const char *description,
                                         wl_shortcut_cb cb, void *ctx)
{
  if (!wl || !wl->shortcuts) return NULL;
  for (struct wl_shortcut *s = wl->shortcut_list; s; s = s->next)
    if (!strcmp(s->id, id)) return s; /* never twice: that is a protocol error */
  struct wl_shortcut *s = xcalloc(1, sizeof *s);
  s->id = xstrdup(id);
  s->cb = cb;
  s->ctx = ctx;
  s->proxy = hyprland_global_shortcuts_manager_v1_register_shortcut(wl->shortcuts, id, app_id, description, "");
  hyprland_global_shortcut_v1_add_listener(s->proxy, &shortcut_listener, s);
  s->next = wl->shortcut_list;
  wl->shortcut_list = s;
  wl_flush(wl);
  return s;
}

void wl_offer_destroy(struct wl_offer *o)
{
  if (!o) return;
  if (o->proxy) ext_data_control_offer_v1_destroy(o->proxy);
  for (size_t i = 0; i < o->n_mimes; i++) free(o->mimes[i]);
  free(o->mimes);
  free(o);
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *proxy, const char *mime)
{
  struct wl_offer *o = data;
  if (o->n_mimes >= MAX_MIMES) return;
  o->mimes = xrealloc(o->mimes, (o->n_mimes + 1) * sizeof *o->mimes);
  o->mimes[o->n_mimes++] = xstrdup(mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = { .offer = offer_mime };

static void device_data_offer(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *proxy)
{
  struct wl_offer *o = xcalloc(1, sizeof *o);
  o->proxy = proxy;
  ext_data_control_offer_v1_add_listener(proxy, &offer_listener, o);
}

static struct wl_offer *offer_of(struct ext_data_control_offer_v1 *proxy)
{
  return proxy ? wl_proxy_get_user_data((struct wl_proxy *)proxy) : NULL;
}

static void device_selection(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *proxy)
{
  struct wl *wl = data;
  /* Ownership of the offer passes to capture; NULL means the clipboard is
   * now empty (its owner went away without handing it on). */
  capture_on_selection(wl->app->capture, offer_of(proxy));
}

static void device_primary_selection(void *data, struct ext_data_control_device_v1 *dev,
                                     struct ext_data_control_offer_v1 *proxy)
{
  struct wl *wl = data;
  capture_on_primary(wl->app->capture, offer_of(proxy));
}

static void device_finished(void *data, struct ext_data_control_device_v1 *dev)
{
  struct wl *wl = data;
  log_err("data-control device finished (seat gone?); exiting so systemd restarts us");
  loop_quit(wl->app->loop);
}

static const struct ext_data_control_device_v1_listener device_listener = {
  .data_offer = device_data_offer,
  .selection = device_selection,
  .finished = device_finished,
  .primary_selection = device_primary_selection,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
  struct wl *wl = data;
  if (!strcmp(iface, wl_seat_interface.name) && !wl->seat)
    wl->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, ext_data_control_manager_v1_interface.name))
    wl->manager = wl_registry_bind(reg, name, &ext_data_control_manager_v1_interface, 1);
  else if (!strcmp(iface, hyprland_global_shortcuts_manager_v1_interface.name))
    wl->shortcuts = wl_registry_bind(reg, name, &hyprland_global_shortcuts_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

/* Before every wait: run anything already queued, then push our requests
 * out. A full socket buffer is retried when the fd becomes writable. */
static void prepare(void *ctx)
{
  struct wl *wl = ctx;
  wl_display_dispatch_pending(wl->display);
  wl_flush(wl);
}

void wl_flush(struct wl *wl)
{
  if (!wl) return;
  int r = wl_display_flush(wl->display);
  bool want = r < 0 && errno == EAGAIN;
  if (want != wl->want_write) {
    wl->want_write = want;
    loop_mod_fd(wl->app->loop, wl->watch, EPOLLIN | (want ? EPOLLOUT : 0));
  }
}

static void on_display(void *ctx, int fd, uint32_t events)
{
  struct wl *wl = ctx;
  if (events & (EPOLLERR | EPOLLHUP)) {
    log_err("Wayland connection lost; exiting");
    loop_quit(wl->app->loop);
    return;
  }
  if (events & EPOLLIN) {
    if (wl_display_dispatch(wl->display) < 0) {
      log_err("Wayland dispatch: %m; exiting");
      loop_quit(wl->app->loop);
      return;
    }
  }
  if (events & EPOLLOUT) wl_flush(wl);
}

struct wl *wl_connect(struct app *app)
{
  struct wl *wl = xcalloc(1, sizeof *wl);
  wl->app = app;
  wl->display = wl_display_connect(NULL);
  if (!wl->display) {
    log_err("cannot connect to the Wayland display (is WAYLAND_DISPLAY set?)");
    free(wl);
    return NULL;
  }
  wl->registry = wl_display_get_registry(wl->display);
  wl_registry_add_listener(wl->registry, &registry_listener, wl);
  wl_display_roundtrip(wl->display);
  if (!wl->seat || !wl->manager) {
    log_err("compositor lacks %s", !wl->seat ? "a seat" : "ext-data-control-v1");
    wl_disconnect(wl);
    return NULL;
  }
  wl->device = ext_data_control_manager_v1_get_data_device(wl->manager, wl->seat);
  ext_data_control_device_v1_add_listener(wl->device, &device_listener, wl);
  wl->watch = loop_add_fd(app->loop, wl_display_get_fd(wl->display), EPOLLIN, on_display, wl);
  loop_set_prepare(app->loop, prepare, wl);
  return wl;
}

void wl_disconnect(struct wl *wl)
{
  if (!wl) return;
  if (wl->watch) loop_del_fd(wl->app->loop, wl->watch);
  while (wl->shortcut_list) {
    struct wl_shortcut *s = wl->shortcut_list;
    wl->shortcut_list = s->next;
    hyprland_global_shortcut_v1_destroy(s->proxy);
    free(s->id);
    free(s);
  }
  if (wl->shortcuts) hyprland_global_shortcuts_manager_v1_destroy(wl->shortcuts);
  if (wl->device) ext_data_control_device_v1_destroy(wl->device);
  if (wl->manager) ext_data_control_manager_v1_destroy(wl->manager);
  if (wl->seat) wl_seat_destroy(wl->seat);
  if (wl->registry) wl_registry_destroy(wl->registry);
  wl_display_flush(wl->display);
  wl_display_disconnect(wl->display);
  free(wl);
}

/* A probe answers "is anything on the clipboard right now?" on its own,
 * short-lived Wayland connection. Every new data-control device receives the
 * current selection, so one device and a sync round trip give the answer.
 *
 * Not on the main connection: destroying a device there races with offers
 * the compositor is already sending it, and libwayland drops events for a
 * destroyed object *including the objects they create*, which desynchronises
 * the id map and kills the connection ("not a valid new object id").
 * Closing a whole connection has no such race. */
struct probe {
  struct wl_seat *seat;
  struct ext_data_control_manager_v1 *manager;
  struct ext_data_control_offer_v1 *offers[8]; /* freed before disconnecting */
  size_t n_offers;
  bool has_selection;
};

static void probe_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
  struct probe *p = data;
  if (!strcmp(iface, wl_seat_interface.name) && !p->seat)
    p->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, ext_data_control_manager_v1_interface.name) && !p->manager)
    p->manager = wl_registry_bind(reg, name, &ext_data_control_manager_v1_interface, 1);
}

static const struct wl_registry_listener probe_registry_listener = {
  .global = probe_global,
  .global_remove = registry_global_remove,
};

static void probe_data_offer(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *o)
{
  struct probe *p = data;
  if (p->n_offers < ARRAY_LEN(p->offers)) p->offers[p->n_offers++] = o;
  else ext_data_control_offer_v1_destroy(o); /* safe: offers never create objects */
}

static void probe_selection(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *o)
{
  struct probe *p = data;
  p->has_selection = o != NULL;
}

static void probe_primary(void *data, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *o) {}
static void probe_finished(void *data, struct ext_data_control_device_v1 *dev) {}

static const struct ext_data_control_device_v1_listener probe_listener = {
  .data_offer = probe_data_offer,
  .selection = probe_selection,
  .finished = probe_finished,
  .primary_selection = probe_primary,
};

static void probe_done(void *data, struct wl_callback *cb, uint32_t serial)
{
  *(bool *)data = true;
}

static const struct wl_callback_listener probe_sync_listener = { .done = probe_done };

/* wl_display_roundtrip with a deadline. */
static bool roundtrip_by(struct wl_display *d, int64_t deadline)
{
  bool done = false;
  struct wl_callback *cb = wl_display_sync(d);
  wl_callback_add_listener(cb, &probe_sync_listener, &done);
  while (!done) {
    while (wl_display_prepare_read(d) != 0) wl_display_dispatch_pending(d);
    if (wl_display_flush(d) < 0 && errno != EAGAIN) { wl_display_cancel_read(d); break; }
    int64_t left = deadline - mono_ms();
    struct pollfd pfd = { .fd = wl_display_get_fd(d), .events = POLLIN };
    if (left <= 0 || poll(&pfd, 1, (int)left) <= 0) { wl_display_cancel_read(d); break; }
    if (wl_display_read_events(d) < 0) break;
    wl_display_dispatch_pending(d);
  }
  wl_callback_destroy(cb);
  return done;
}

void wl_probe_selection(struct wl *wl, wl_probe_cb cb, void *ctx)
{
  /* Unknown means "occupied": never overwrite a clipboard on a guess. */
  bool has = true;
  struct wl_display *d = wl_display_connect(NULL);
  if (d) {
    int64_t deadline = mono_ms() + 250;
    struct probe p = { 0 };
    struct wl_registry *reg = wl_display_get_registry(d);
    wl_registry_add_listener(reg, &probe_registry_listener, &p);
    struct ext_data_control_device_v1 *dev = NULL;
    if (roundtrip_by(d, deadline) && p.seat && p.manager) {
      dev = ext_data_control_manager_v1_get_data_device(p.manager, p.seat);
      ext_data_control_device_v1_add_listener(dev, &probe_listener, &p);
      if (roundtrip_by(d, deadline)) has = p.has_selection;
    }
    /* Everything goes with the connection; nothing more is read from it. */
    for (size_t i = 0; i < p.n_offers; i++) ext_data_control_offer_v1_destroy(p.offers[i]);
    if (dev) ext_data_control_device_v1_destroy(dev);
    if (p.manager) ext_data_control_manager_v1_destroy(p.manager);
    if (p.seat) wl_seat_destroy(p.seat);
    wl_registry_destroy(reg);
    wl_display_disconnect(d);
  }
  cb(ctx, has);
}

void wl_offer_receive(struct wl *wl, struct wl_offer *o, const char *mime, int fd)
{
  ext_data_control_offer_v1_receive(o->proxy, mime, fd);
  wl_flush(wl);
}

struct ext_data_control_source_v1 *wl_source_create(struct wl *wl, const struct ext_data_control_source_v1_listener *l,
                                                    void *data)
{
  struct ext_data_control_source_v1 *s = ext_data_control_manager_v1_create_data_source(wl->manager);
  ext_data_control_source_v1_add_listener(s, l, data);
  return s;
}

void wl_source_offer(struct ext_data_control_source_v1 *s, const char *mime)
{
  ext_data_control_source_v1_offer(s, mime);
}

void wl_set_selection(struct wl *wl, struct ext_data_control_source_v1 *s)
{
  ext_data_control_device_v1_set_selection(wl->device, s);
  wl_flush(wl);
}

void wl_source_destroy(struct ext_data_control_source_v1 *s)
{
  if (s) ext_data_control_source_v1_destroy(s);
}

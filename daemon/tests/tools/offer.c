/*
 * Test helper: offer stdin on the clipboard under every type named on the
 * command line (wl-copy offers one type per call), until another client
 * takes the clipboard or `timeout` expires.
 *
 *   offer [--timeout MS] [--primary] TYPE... < payload
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <wayland-client.h>

#include "ext-data-control-v1.h"

static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *mgr;
static char *payload;
static size_t payload_len;
static int done;

static void on_send(void *d, struct ext_data_control_source_v1 *s, const char *mime, int fd)
{
  size_t off = 0;
  while (off < payload_len) {
    ssize_t w = write(fd, payload + off, payload_len - off);
    if (w <= 0) break;
    off += (size_t)w;
  }
  close(fd);
}

static void on_cancelled(void *d, struct ext_data_control_source_v1 *s) { done = 1; }

static const struct ext_data_control_source_v1_listener src_l = { on_send, on_cancelled };

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
  if (!strcmp(iface, wl_seat_interface.name) && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
  if (!strcmp(iface, ext_data_control_manager_v1_interface.name))
    mgr = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_l = { global, global_remove };

int main(int argc, char **argv)
{
  int timeout = 5000, i = 1;
  bool primary = false;
  for (;;) {
    if (i + 1 < argc && !strcmp(argv[i], "--timeout")) { timeout = atoi(argv[i + 1]); i += 2; }
    else if (i < argc && !strcmp(argv[i], "--primary")) { primary = true; i++; }
    else break;
  }
  if (i >= argc) { fprintf(stderr, "usage: offer [--timeout MS] TYPE... < payload\n"); return 2; }
  size_t cap = 0;
  for (;;) {
    if (payload_len == cap) payload = realloc(payload, cap = cap ? cap * 2 : 65536);
    ssize_t r = read(0, payload + payload_len, cap - payload_len);
    if (r <= 0) break;
    payload_len += (size_t)r;
  }
  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) return 2;
  wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
  wl_display_roundtrip(dpy);
  if (!seat || !mgr) return 2;
  struct ext_data_control_device_v1 *dev = ext_data_control_manager_v1_get_data_device(mgr, seat);
  struct ext_data_control_source_v1 *src = ext_data_control_manager_v1_create_data_source(mgr);
  for (; i < argc; i++) ext_data_control_source_v1_offer(src, argv[i]);
  ext_data_control_source_v1_add_listener(src, &src_l, NULL);
  if (primary) ext_data_control_device_v1_set_primary_selection(dev, src);
  else ext_data_control_device_v1_set_selection(dev, src);
  wl_display_flush(dpy);
  struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
  while (!done) {
    wl_display_flush(dpy);
    int r = poll(&p, 1, timeout);
    if (r <= 0) break;
    if (wl_display_dispatch(dpy) < 0) break;
  }
  return 0;
}

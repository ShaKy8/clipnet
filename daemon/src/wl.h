/*
 * Wayland side: one data-control device on the first seat.
 *
 * ext-data-control-v1 lets a client without a focused surface read and set
 * the clipboard. Offers arrive as data_offer (a new object) followed by its
 * mime types, then selection() names which offer is now the clipboard.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct app;
struct wl;
struct ext_data_control_offer_v1;
struct ext_data_control_source_v1;
struct ext_data_control_source_v1_listener;

struct wl_offer {
  struct ext_data_control_offer_v1 *proxy;
  char **mimes;
  size_t n_mimes;
};

struct wl *wl_connect(struct app *app);
void wl_disconnect(struct wl *wl);
void wl_offer_destroy(struct wl_offer *o);

/* Ask the source of an offer to write one type into fd (then close fd). */
void wl_offer_receive(struct wl *wl, struct wl_offer *o, const char *mime, int fd);

struct ext_data_control_source_v1 *wl_source_create(struct wl *wl, const struct ext_data_control_source_v1_listener *l,
                                                    void *data);
void wl_source_offer(struct ext_data_control_source_v1 *s, const char *mime);
void wl_set_selection(struct wl *wl, struct ext_data_control_source_v1 *s);
void wl_source_destroy(struct ext_data_control_source_v1 *s);
void wl_flush(struct wl *wl);

/* Hyprland global shortcuts: the compositor calls us when a bind that
 * dispatches hl.dsp.global("<app_id>:<id>") is pressed. Registering an
 * app_id + id twice on one connection is a fatal protocol error, so callers
 * register each id once and keep it. NULL when the protocol is missing. */
struct wl_shortcut;
typedef void (*wl_shortcut_cb)(void *ctx, const char *id);
struct wl_shortcut *wl_shortcut_register(struct wl *wl, const char *app_id, const char *id, const char *description,
                                         wl_shortcut_cb cb, void *ctx);
bool wl_has_shortcuts(struct wl *wl);

/* Ask the compositor whether anything is on the clipboard right now. The
 * main device is not told when a clipboard owner exits (Hyprland 0.56 sends
 * no selection(NULL) then), but a newly created device always receives the
 * current selection, so a throwaway device answers the question. */
typedef void (*wl_probe_cb)(void *ctx, bool has_selection);
void wl_probe_selection(struct wl *wl, wl_probe_cb cb, void *ctx);

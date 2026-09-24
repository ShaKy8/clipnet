/*
 * Hyprland: the request socket (queries, Lua eval) and the event socket
 * (which window is focused, config reloads).
 *
 * The request socket answers in well under a millisecond, so requests are
 * made synchronously with a short timeout. The event socket is read on the
 * loop, so the focused app is always known without asking.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

typedef struct cJSON cJSON;
struct app;
struct hypr;

/* NULL when not running under Hyprland. */
struct hypr *hypr_connect(struct app *app);
void hypr_disconnect(struct hypr *h);

/* Class and title of the focused window (NULL if none). */
const char *hypr_active_class(struct hypr *h);
const char *hypr_active_title(struct hypr *h);

/* Raw request ("j/cursorpos", "eval <lua>", ...). Reply is malloc'd. */
char *hypr_request(struct hypr *h, const char *cmd, size_t *len);
/* Parsed JSON reply of a "j/..." request, or NULL. */
cJSON *hypr_query(struct hypr *h, const char *what);
/* Run a Lua chunk inside Hyprland. 0 when Hyprland answered "ok". */
int hypr_eval(struct hypr *h, const char *lua);

/* Called on every configreloaded event (binds and Lua globals are gone). */
typedef void (*hypr_reload_cb)(void *ctx);
void hypr_on_reload(struct hypr *h, hypr_reload_cb cb, void *ctx);

/* Called when a window closes or focus moves: the moments a clipboard owner
 * may have just gone away. */
typedef void (*hypr_window_cb)(void *ctx);
void hypr_on_window_change(struct hypr *h, hypr_window_cb cb, void *ctx);

/* A per-app override of the paste keystroke. */
struct paste_key {
  const char *pattern; /* case-insensitive glob over the window class */
  const char *mods;    /* e.g. "CTRL SHIFT" */
  const char *key;     /* e.g. "V", "Insert" */
};

/* Send the paste keystroke to whatever window has focus once our popup is
 * gone and no modifier is held: overrides first, then SHIFT+Insert for
 * windows tagged "terminal" (Omarchy's convention), else CTRL+V. */
int hypr_send_paste(struct hypr *h, int delay_ms, const struct paste_key *overrides, size_t n);
/* Same machinery for another chord (copy buffers send CTRL+C / CTRL+X). */
int hypr_send_keys(struct hypr *h, int delay_ms, const char *mods, const char *key, const char *term_mods,
                   const char *term_key);

/* Escape s as a Lua long-bracket-free double-quoted string literal. */
void lua_quote(struct buf *b, const char *s);

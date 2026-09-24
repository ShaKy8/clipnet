/*
 * Capture: a new clipboard selection becomes a stored clip.
 *
 *   selection(offer) → skip checks (our own paste, password hint, paused,
 *   excluded app) → 30 ms debounce → read each planned format in turn through
 *   a non-blocking pipe → store → clip.added.
 *
 * Reads never block the loop: each pipe is watched like any other fd, with a
 * per-format timeout and a size cap. A newer selection aborts a capture that
 * is still reading.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct app;
struct capture;
struct wl_offer;

struct capture *capture_new(struct app *app);
void capture_free(struct capture *c);

/* Takes ownership of offer (may be NULL: clipboard emptied). */
void capture_on_selection(struct capture *c, struct wl_offer *offer);
void capture_on_primary(struct capture *c, struct wl_offer *offer);

/* The clip whose content is on the clipboard right now, as far as we know:
 * the last one captured or served. 0 when unknown or sensitive. */
int64_t capture_current_clip(struct capture *c);
void capture_set_current_clip(struct capture *c, int64_t id);

/* Something happened that may have taken the clipboard owner away (a window
 * closed, focus moved). Checks shortly after and, if the clipboard is empty,
 * puts the last captured clip back (the keep_alive setting). */
void capture_owner_may_have_left(struct capture *c);

/* Route the next captured selection (within timeout_ms) to a callback
 * instead of treating it as ordinary history; used by copy buffers. */
typedef void (*capture_hook)(void *ctx, int64_t clip_id);
void capture_expect(struct capture *c, int timeout_ms, capture_hook hook, void *ctx);

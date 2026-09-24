/*
 * Serve: own the clipboard and hand out payloads.
 *
 * Every format of a clip is offered again under the names it was captured
 * with, plus a private marker carrying the clip's uuid so capture can tell
 * our own selection apart from a real copy. Readers are fed from memory or
 * from mapped blob files in 64 KiB non-blocking writes; any number can be in
 * flight, and a reader that stalls is dropped after a timeout.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app;
struct serve;

enum serve_mode {
  SERVE_ALL,   /* every stored format */
  SERVE_PLAIN, /* the clip's text only (Ditto's "paste as plain text") */
};

struct serve *serve_new(struct app *app);
void serve_free(struct serve *s);

/* Own the clipboard with a stored clip. 0 on success. */
int serve_clip(struct serve *s, int64_t id, enum serve_mode mode);
/* Own the clipboard with arbitrary text (a transform, a multi-clip join).
 * clip_id, when nonzero, is reported as the current clip. */
int serve_text(struct serve *s, const char *text, size_t len, int64_t clip_id);
/* The id of the clip we are serving, or 0. */
int64_t serve_current(struct serve *s);
/* Number of payload transfers still being written. */
int serve_pending_writes(struct serve *s);

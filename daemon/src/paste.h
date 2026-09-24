/*
 * Paste: put clips on the clipboard and (optionally) press the paste key in
 * the window that had focus before the popup.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "serve.h"

struct app;

struct paste_req {
  const int64_t *ids;
  size_t n_ids;
  enum serve_mode mode;
  const char *separator; /* joining several clips; NULL: the setting */
  const char *text;      /* when set, paste this text instead of the clips' own
                            (a Special Paste transform), still credited to ids */
  bool send_keys;        /* false: only put it on the clipboard (Ctrl+C) */
};

/* 0 on success; on failure *err is a short static message. */
int paste_run(struct app *app, const struct paste_req *req, const char **err);

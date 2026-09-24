/*
 * Rules: per-app and per-format policy, stored in the rules table and
 * applied in `ord` order.
 *
 *   exclude     match_app                      don't record copies from the app
 *   skip_mime   match_mime (+ match_app)       drop matching formats from copies
 *   paste_keys  match_app, arg {"keys": "Ctrl+Shift+V"}
 *                                              press this to paste in the app
 *   to_group    match_app, arg {"group": uuid} file the app's copies in a group
 *
 * Patterns are case-insensitive shell globs. Apps are matched by the class of
 * the window that had focus when the copy happened (Hyprland's "class").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;
struct db;
struct mime_plan;

/* True when an enabled "exclude" rule matches app. NULL app never matches
 * (a copy from a script or a background process has no focused-app blame). */
bool rules_excluded(struct db *db, const char *app);

/* Remove the reads that skip_mime rules drop for this app. */
void rules_filter_plan(struct db *db, const char *app, struct mime_plan *plan);

/* The group a to_group rule sends this app's copies to, or 0. */
int64_t rules_route_group(struct db *db, const char *app);

struct paste_key;
/* Enabled paste_keys rules in order. Returns the count; free with
 * rules_paste_keys_free. */
size_t rules_paste_keys(struct db *db, struct paste_key **out);
void rules_paste_keys_free(struct paste_key *keys, size_t n);

/* ---- editing ------------------------------------------------------------ */

/* [{id, enabled, action, match_app, match_mime, arg}] in order. */
cJSON *rules_list(struct db *db);
/* Create (id 0) or replace a rule from {enabled?, action, match_app?,
 * match_mime?, arg?}. Returns the id, or -1 with *err set. */
int64_t rules_set(struct db *db, int64_t id, const cJSON *spec, const char **err);
int rules_delete(struct db *db, int64_t id, const char **err);
int rules_reorder(struct db *db, const int64_t *ids, size_t n, const char **err);

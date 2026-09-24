/*
 * Rules: per-app and per-type policy, stored in the rules table.
 *
 * Patterns are case-insensitive shell globs matched against the class of the
 * window that was focused when the copy happened (Hyprland's "class").
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct db;

/* True when an enabled "exclude" rule matches app. NULL app never matches
 * (a copy from a script or a background process has no focused-app blame). */
bool rules_excluded(struct db *db, const char *app);

struct paste_key;
/* Enabled "paste_keys" rules in order, arg {"mods": "CTRL SHIFT", "key": "V"}.
 * Returns the count; free with rules_paste_keys_free. */
size_t rules_paste_keys(struct db *db, struct paste_key **out);
void rules_paste_keys_free(struct paste_key *keys, size_t n);

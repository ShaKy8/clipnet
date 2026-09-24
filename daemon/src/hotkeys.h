/*
 * Global hotkeys owned by the daemon: "paste this clip" (Ditto's per-clip
 * shortcut) and "paste history position N".
 *
 * Each hotkey row is registered once as a Hyprland global shortcut
 * "clipnetd:h<id>", and bound with hl.bind from one Lua chunk that first
 * removes every bind it made before. A config reload wipes runtime binds, so
 * the chunk runs again on every configreloaded event.
 */
#pragma once

#include <stdint.h>

typedef struct cJSON cJSON;
struct app;
struct hotkeys;

struct hotkeys *hotkeys_new(struct app *app);
void hotkeys_free(struct hotkeys *hk);

/* Re-apply every enabled hotkey to Hyprland. */
void hotkeys_sync(struct hotkeys *hk);
/* Drop hotkeys whose clip no longer exists; re-sync if any went. */
void hotkeys_prune(struct hotkeys *hk);

/* [{id, accel, action, arg, enabled, label, conflict}] */
cJSON *hotkeys_list(struct hotkeys *hk);

/* Create a hotkey, or re-point it when id is nonzero. For paste_clip the
 * clip's previous hotkey, if any, is replaced (one per clip). A key already
 * bound in Hyprland is refused with *err naming what it does.
 * Returns the hotkey id, or -1. *err may point into *err_buf (caller frees). */
int64_t hotkeys_set(struct hotkeys *hk, int64_t id, const char *accel, const char *action, const char *arg,
                    const char **err, char **err_buf);
int hotkeys_remove(struct hotkeys *hk, int64_t id, const char **err);

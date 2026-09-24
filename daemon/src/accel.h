/*
 * Keyboard shortcuts in a portable spelling ("Ctrl+Alt+1", "Super+Shift+V",
 * "Ctrl+Apostrophe"), stored as such so a macOS build can map them its own
 * way, and translated to Hyprland ("CTRL + ALT + 1") only when bound.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ACCEL_SHIFT 1u
#define ACCEL_CTRL 4u
#define ACCEL_ALT 8u
#define ACCEL_SUPER 64u /* the same bit values as Hyprland's modmask */

struct accel {
  uint32_t mods;
  char key[32]; /* canonical portable key name, e.g. "A", "1", "F5", "Apostrophe" */
};

/* Parse and normalise. Needs at least one modifier unless the key is F1–F24.
 * On failure returns false with *err set. */
bool accel_parse(const char *s, struct accel *out, const char **err);
/* For keys we *send* rather than bind (per-app paste keys): Shift alone and
 * a bare key are fine there ("Shift+Insert"). */
bool accel_parse_send(const char *s, struct accel *out, const char **err);
/* The modifiers as Hyprland's send_key_state wants them: "CTRL SHIFT", or
 * "" for none. out must hold 32 bytes. */
void accel_hypr_mods(const struct accel *a, char out[32]);
/* The key's keysym name ("V", "Insert", "apostrophe"). */
const char *accel_keysym(const struct accel *a);
/* "Ctrl+Alt+1" (canonical order: Super, Ctrl, Alt, Shift). */
void accel_format(const struct accel *a, char out[64]);
/* "CTRL + ALT + 1" for hl.bind. */
void accel_to_hypr(const struct accel *a, char out[96]);
/* Does a Hyprland bind (modmask + key as Hyprland reports it) press the same
 * keys? Key names compare case-insensitively via their keysym spelling. */
bool accel_matches_hypr(const struct accel *a, uint32_t modmask, const char *hypr_key);

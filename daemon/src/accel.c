#include "accel.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "util.h"

/* Portable name, xkb keysym name (what Hyprland binds by), other spellings. */
static const struct {
  const char *name;
  const char *keysym;
  const char *alias1, *alias2;
} named[] = {
  { "Apostrophe", "apostrophe", "'", "Quote" },
  { "Grave", "grave", "`", "Backtick" },
  { "Comma", "comma", ",", NULL },
  { "Period", "period", ".", "Dot" },
  { "Slash", "slash", "/", NULL },
  { "Backslash", "backslash", "\\", NULL },
  { "Semicolon", "semicolon", ";", NULL },
  { "Minus", "minus", "-", NULL },
  { "Equal", "equal", "=", "Equals" },
  { "BracketLeft", "bracketleft", "[", NULL },
  { "BracketRight", "bracketright", "]", NULL },
  { "Space", "space", NULL, NULL },
  { "Tab", "Tab", NULL, NULL },
  { "Return", "Return", "Enter", NULL },
  { "Escape", "Escape", "Esc", NULL },
  { "Backspace", "BackSpace", NULL, NULL },
  { "Delete", "Delete", "Del", NULL },
  { "Insert", "Insert", "Ins", NULL },
  { "Home", "Home", NULL, NULL },
  { "End", "End", NULL, NULL },
  { "PageUp", "Prior", "Prior", "PgUp" },
  { "PageDown", "Next", "Next", "PgDown" },
  { "Left", "Left", NULL, NULL },
  { "Right", "Right", NULL, NULL },
  { "Up", "Up", NULL, NULL },
  { "Down", "Down", NULL, NULL },
  { "Print", "Print", "PrintScreen", NULL },
};

static bool is_fkey(const char *k, int *num)
{
  if ((k[0] != 'F' && k[0] != 'f') || !isdigit((unsigned char)k[1])) return false;
  int n = 0;
  for (const char *p = k + 1; *p; p++) {
    if (!isdigit((unsigned char)*p)) return false;
    n = n * 10 + (*p - '0');
  }
  if (num) *num = n;
  return n >= 1 && n <= 24;
}

/* Canonical portable name for a key token, or false. */
static bool canon_key(const char *tok, char out[32])
{
  size_t n = strlen(tok);
  if (n == 1 && isalnum((unsigned char)tok[0])) {
    out[0] = (char)toupper((unsigned char)tok[0]);
    out[1] = 0;
    return true;
  }
  int f;
  if (is_fkey(tok, &f)) {
    snprintf(out, 32, "F%d", f);
    return true;
  }
  for (size_t i = 0; i < ARRAY_LEN(named); i++) {
    if (!strcasecmp(tok, named[i].name) || !strcasecmp(tok, named[i].keysym) ||
        (named[i].alias1 && !strcasecmp(tok, named[i].alias1)) || (named[i].alias2 && !strcasecmp(tok, named[i].alias2))) {
      snprintf(out, 32, "%s", named[i].name);
      return true;
    }
  }
  return false;
}

static const char *keysym_of(const char *canon)
{
  for (size_t i = 0; i < ARRAY_LEN(named); i++)
    if (!strcmp(canon, named[i].name)) return named[i].keysym;
  return canon; /* letters, digits, F-keys are their own keysym names */
}

static uint32_t mod_bit(const char *tok)
{
  if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) return ACCEL_CTRL;
  if (!strcasecmp(tok, "Alt") || !strcasecmp(tok, "Mod1")) return ACCEL_ALT;
  if (!strcasecmp(tok, "Shift")) return ACCEL_SHIFT;
  if (!strcasecmp(tok, "Super") || !strcasecmp(tok, "Meta") || !strcasecmp(tok, "Win") || !strcasecmp(tok, "Mod4"))
    return ACCEL_SUPER;
  return 0;
}

bool accel_parse(const char *s, struct accel *out, const char **err)
{
  memset(out, 0, sizeof *out);
  char buf[128];
  if (!s || strlen(s) >= sizeof buf) { *err = "not a shortcut"; return false; }
  snprintf(buf, sizeof buf, "%s", s);

  /* Split on '+', but a trailing "+" key ("Ctrl++") is not supported: the
   * plus key is "Equal" with Shift on most layouts anyway. */
  char *tokens[8];
  size_t n = 0;
  char *save = NULL;
  for (char *t = strtok_r(buf, "+", &save); t && n < ARRAY_LEN(tokens); t = strtok_r(NULL, "+", &save)) {
    while (*t == ' ') t++;
    size_t len = strlen(t);
    while (len && t[len - 1] == ' ') t[--len] = 0;
    if (len) tokens[n++] = t;
  }
  if (!n) { *err = "empty shortcut"; return false; }
  for (size_t i = 0; i + 1 < n; i++) {
    uint32_t bit = mod_bit(tokens[i]);
    if (!bit) { *err = "unknown modifier (use Ctrl, Alt, Shift or Super)"; return false; }
    out->mods |= bit;
  }
  if (mod_bit(tokens[n - 1])) { *err = "a shortcut needs a key after the modifiers"; return false; }
  if (!canon_key(tokens[n - 1], out->key)) { *err = "unsupported key"; return false; }
  if (!out->mods && !is_fkey(out->key, NULL)) { *err = "add a modifier (Ctrl, Alt, Shift or Super)"; return false; }
  if (out->mods == ACCEL_SHIFT && !is_fkey(out->key, NULL)) {
    *err = "Shift alone would stop you typing that key; add Ctrl, Alt or Super";
    return false;
  }
  return true;
}

void accel_format(const struct accel *a, char out[64])
{
  snprintf(out, 64, "%s%s%s%s%s", a->mods & ACCEL_SUPER ? "Super+" : "", a->mods & ACCEL_CTRL ? "Ctrl+" : "",
           a->mods & ACCEL_ALT ? "Alt+" : "", a->mods & ACCEL_SHIFT ? "Shift+" : "", a->key);
}

void accel_to_hypr(const struct accel *a, char out[96])
{
  snprintf(out, 96, "%s%s%s%s%s", a->mods & ACCEL_SUPER ? "SUPER + " : "", a->mods & ACCEL_CTRL ? "CTRL + " : "",
           a->mods & ACCEL_ALT ? "ALT + " : "", a->mods & ACCEL_SHIFT ? "SHIFT + " : "", keysym_of(a->key));
}

bool accel_matches_hypr(const struct accel *a, uint32_t modmask, const char *hypr_key)
{
  /* Caps Lock (2) and Num Lock (Mod2, 16) do not change which bind fires. */
  const uint32_t relevant = ACCEL_SHIFT | ACCEL_CTRL | ACCEL_ALT | ACCEL_SUPER;
  if ((modmask & relevant) != a->mods) return false;
  char k[32];
  if (!canon_key(hypr_key, k)) return false;
  return !strcmp(k, a->key);
}

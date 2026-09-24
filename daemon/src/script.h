/*
 * User scripts (Ditto has ChaiScript; CLIP//NET has Lua 5.4).
 *
 * Scripts are the .lua files in $XDG_CONFIG_HOME/clipnet/scripts that define
 * either or both of:
 *
 *   function on_copy(clip)          -- a new copy, before it is stored
 *     -- return nil (keep as is), false (don't store it), or a table of
 *     -- changes: { text=, title=, group="Work/Snippets", sticky=, locked= }
 *   end
 *
 *   function on_paste(clip, target) -- just before a paste
 *     -- return nil (paste as is) or a string to paste instead
 *   end
 *
 * clip = { kind, text, mimes = {...}, size, source = { app, title },
 *          title, get = function(mime) -> bytes or nil }
 * target = { app }
 *
 * Every script runs in its own sandbox: no io, os.execute, require or
 * binary chunks; 64 MiB of memory and 100 ms per call. A script starts
 * disabled until the user enables it. docs/SCRIPTING.md has the details.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;
struct app;
struct scripts;
struct fmt_in;

struct scripts *scripts_new(struct app *app, const char *dir);
void scripts_free(struct scripts *s);
const char *scripts_dir(struct scripts *s);

/* What on_copy decided. */
struct copy_verdict {
  bool skip;
  char *text;     /* replacement text (the clip becomes plain text), or NULL */
  char *title;    /* NULL: unchanged */
  char *group;    /* "Parent/Child" path, created if missing; NULL: none */
  int sticky;     /* -1 unchanged, 0/1 */
  int locked;     /* -1 unchanged, 0/1 */
};
void copy_verdict_free(struct copy_verdict *v);

/* Run every enabled on_copy in order. Each sees the previous one's text. */
void scripts_on_copy(struct scripts *s, const struct fmt_in *fmts, size_t n, const char *source_app,
                     const char *source_title, struct copy_verdict *out);
/* Is any enabled script defining on_paste? (Cheap: lets paste skip loading
 * the clip's text when nothing would look at it.) */
bool scripts_have_paste(struct scripts *s);
/* Run every enabled on_paste in order over text. Returns a malloc'd
 * replacement, or NULL when no script changed it. */
char *scripts_on_paste(struct scripts *s, int64_t clip_id, const char *text, size_t len, const char *target_app);

/* [{name, enabled, hooks: ["on_copy", ...], error: string|null}] */
cJSON *scripts_list(struct scripts *s);
int scripts_enable(struct scripts *s, const char *name, bool enabled, const char **err);
/* Drop every loaded state; files are re-read on next use. */
void scripts_reload(struct scripts *s);
/* Dry run one script's hook on a stored clip: {result, log: [...], ms, error}. */
cJSON *scripts_test(struct scripts *s, const char *name, const char *hook, int64_t clip_id, const char **err);
/* Copy the bundled examples into the scripts directory (never overwriting). */
cJSON *scripts_install_examples(struct scripts *s, const char *examples_dir, const char **err);

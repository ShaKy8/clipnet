/*
 * Clipboard format policy: which offered types to read, what to call them,
 * and what kind of clip they add up to.
 *
 * Text is offered under many names (UTF8_STRING, text/plain, STRING, ...).
 * It is read once under the best name, stored as MIME_TEXT, and the names the
 * source used are kept so a paste can offer exactly the same set again.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MIME_TEXT "text/plain;charset=utf-8"
#define MIME_MARKER "application/x-clipnet-clip"
#define MIME_PASSWORD_HINT "x-kde-passwordManagerHint"
#define MIME_HTML "text/html"
#define MIME_URI_LIST "text/uri-list"

/* The names offered for text when a clip has none recorded (a created clip,
 * an import, a transformed paste). Order matters: best first. */
extern const char *const mime_text_default_aliases[];
extern const size_t mime_text_default_alias_count;

enum clip_kind { KIND_TEXT, KIND_RICH, KIND_IMAGE, KIND_FILES, KIND_OTHER };
const char *kind_name(enum clip_kind k);

bool mime_is_text_alias(const char *m);
bool mime_is_image(const char *m);
/* Protocol plumbing and our own marker: never stored. */
bool mime_is_ignored(const char *m);

struct mime_read {
  const char *offered;   /* name to request from the source */
  const char *canonical; /* name to store it under */
};

struct mime_plan {
  struct mime_read reads[32];
  size_t n_reads;
  const char *text_aliases[16]; /* offered text names, in offer order */
  size_t n_text_aliases;
  bool is_ours;          /* our own paste coming back */
  bool is_sensitive;     /* password manager hint present */
};

/* Decide what to read from an offer. Pointers alias the offered strings. */
void mime_plan_build(struct mime_plan *p, const char *const *offered, size_t n);

/* Kind from the set of canonical mimes a clip ended up with. */
enum clip_kind mime_classify(const char *const *mimes, size_t n);

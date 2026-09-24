#include "mime.h"

#include <strings.h>
#include <string.h>

#include "util.h"

const char *const mime_text_default_aliases[] = {
  MIME_TEXT, "text/plain", "UTF8_STRING", "STRING", "TEXT",
};
const size_t mime_text_default_alias_count = ARRAY_LEN(mime_text_default_aliases);

const char *kind_name(enum clip_kind k)
{
  switch (k) {
  case KIND_TEXT: return "text";
  case KIND_RICH: return "rich";
  case KIND_IMAGE: return "image";
  case KIND_FILES: return "files";
  default: return "other";
  }
}

/* Lower is better. -1: not a text alias. text/plain without a charset is
 * UTF-8 in practice on Wayland (every toolkit writes UTF-8 there), so it
 * ranks with UTF8_STRING; STRING and TEXT are Latin-1 from X11 clients. */
static int text_rank(const char *m)
{
  if (!strcasecmp(m, "text/plain;charset=utf-8") || !strcasecmp(m, "text/plain; charset=utf-8")) return 0;
  if (!strcmp(m, "UTF8_STRING")) return 1;
  if (!strcmp(m, "text/plain")) return 2;
  if (!strcmp(m, "STRING")) return 3;
  if (!strcmp(m, "TEXT")) return 4;
  return -1;
}

bool mime_is_text_alias(const char *m)
{
  return text_rank(m) >= 0;
}

bool mime_is_image(const char *m)
{
  return !strncmp(m, "image/", 6);
}

bool mime_is_ignored(const char *m)
{
  static const char *const ignored[] = {
    "TARGETS", "MULTIPLE", "TIMESTAMP", "SAVE_TARGETS", "DELETE", "INCR",
    MIME_MARKER, MIME_PASSWORD_HINT,
  };
  for (size_t i = 0; i < ARRAY_LEN(ignored); i++)
    if (!strcmp(m, ignored[i])) return true;
  return false;
}

static bool seen(const struct mime_plan *p, const char *canonical)
{
  for (size_t i = 0; i < p->n_reads; i++)
    if (!strcmp(p->reads[i].canonical, canonical)) return true;
  return false;
}

void mime_plan_build(struct mime_plan *p, const char *const *offered, size_t n)
{
  memset(p, 0, sizeof *p);
  const char *best_text = NULL;
  int best_rank = 99;
  bool has_png = false;

  for (size_t i = 0; i < n; i++) {
    const char *m = offered[i];
    if (!strcmp(m, MIME_MARKER)) p->is_ours = true;
    if (!strcmp(m, MIME_PASSWORD_HINT)) p->is_sensitive = true;
    if (!strcmp(m, "image/png")) has_png = true;
    int r = text_rank(m);
    if (r >= 0) {
      bool dup = false;
      for (size_t k = 0; k < p->n_text_aliases; k++)
        if (!strcmp(p->text_aliases[k], m)) dup = true;
      if (!dup && p->n_text_aliases < ARRAY_LEN(p->text_aliases)) p->text_aliases[p->n_text_aliases++] = m;
      if (r < best_rank) { best_rank = r; best_text = m; }
    }
  }

  /* Text first: it is what search, preview and plain paste are built on. */
  if (best_text) p->reads[p->n_reads++] = (struct mime_read){ best_text, MIME_TEXT };

  for (size_t i = 0; i < n && p->n_reads < ARRAY_LEN(p->reads); i++) {
    const char *m = offered[i];
    if (mime_is_ignored(m) || mime_is_text_alias(m)) continue;
    /* Toolkits offer one picture as a dozen converted formats. The PNG is
     * lossless and universally accepted; the rest would multiply storage.
     * SVG is a different source document, not a conversion, so it stays. */
    if (has_png && mime_is_image(m) && strcmp(m, "image/png") && strcmp(m, "image/svg+xml")) continue;
    if (seen(p, m)) continue;
    p->reads[p->n_reads++] = (struct mime_read){ m, m };
  }
}

enum clip_kind mime_classify(const char *const *mimes, size_t n)
{
  bool text = false, rich = false, image = false, files = false;
  for (size_t i = 0; i < n; i++) {
    const char *m = mimes[i];
    if (!strcmp(m, MIME_TEXT)) text = true;
    else if (!strcmp(m, MIME_HTML) || !strcmp(m, "text/rtf") || !strcmp(m, "application/rtf")) rich = true;
    else if (mime_is_image(m)) image = true;
    else if (!strcmp(m, MIME_URI_LIST) || !strcmp(m, "x-special/gnome-copied-files")) files = true;
  }
  /* Files beat images (a copied .png file also offers its bytes in some file
   * managers); images beat rich text (a browser image copy carries an <img>
   * HTML fragment too). */
  if (files) return KIND_FILES;
  if (image) return KIND_IMAGE;
  if (rich) return KIND_RICH;
  if (text) return KIND_TEXT;
  return KIND_OTHER;
}

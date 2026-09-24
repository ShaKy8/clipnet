#include "paste.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "db.h"
#include "hypr.h"
#include "rules.h"
#include "settings.h"
#include "util.h"

int paste_run(struct app *app, const struct paste_req *req, const char **err)
{
  if (!req->n_ids && !req->text) { *err = "nothing to paste"; return -1; }
  for (size_t i = 0; i < req->n_ids; i++) {
    if (!db_exists(app->db, req->ids[i])) { *err = "no such clip"; return -1; }
  }
  if (!app->wl) { *err = "no Wayland connection"; return -1; }

  int rc;
  int64_t credit = req->n_ids == 1 ? req->ids[0] : 0;
  if (req->text) {
    rc = serve_text(app->serve, req->text, strlen(req->text), credit);
  } else if (req->n_ids == 1) {
    rc = serve_clip(app->serve, req->ids[0], req->mode);
  } else {
    /* Several clips paste as one text, in the order given, joined by the
     * separator (Ditto pastes multi-selections the same way). */
    char *sep = req->separator ? xstrdup(req->separator) : setting_string(app->db, "multi_separator");
    struct buf joined = { 0 };
    for (size_t i = 0; i < req->n_ids; i++) {
      size_t len;
      char *t = db_clip_text(app->db, req->ids[i], &len);
      if (!t) continue; /* an image in a multi-selection has no text part */
      if (joined.len) buf_append_str(&joined, sep);
      buf_append(&joined, t, len);
      free(t);
    }
    free(sep);
    if (!joined.len) {
      buf_free(&joined);
      *err = "the selected clips have no text";
      return -1;
    }
    rc = serve_text(app->serve, (char *)joined.data, joined.len, 0);
    buf_free(&joined);
  }
  if (rc < 0) { *err = "could not put the clip on the clipboard"; return -1; }

  if (setting_bool(app->db, "move_to_top_on_paste")) {
    for (size_t i = 0; i < req->n_ids; i++) {
      db_touch(app->db, req->ids[i], req->send_keys);
      app_emit_clip(app, "clip.updated", req->ids[i]);
    }
  }

  if (req->send_keys) {
    if (!app->hypr) { *err = "not running under Hyprland; clip copied but not pasted"; return -1; }
    struct paste_key *keys = NULL;
    size_t n = rules_paste_keys(app->db, &keys);
    rc = hypr_send_paste(app->hypr, (int)setting_int(app->db, "paste_delay_ms"), keys, n);
    rules_paste_keys_free(keys, n);
    if (rc < 0) { *err = "Hyprland refused the paste keystroke"; return -1; }
  }
  return 0;
}

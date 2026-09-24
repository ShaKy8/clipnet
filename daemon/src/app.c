#include "app.h"

#include <stdlib.h>

#include "cJSON.h"
#include "db.h"
#include "ipc.h"
#include "loop.h"
#include "settings.h"
#include "util.h"

void app_emit(struct app *app, const char *event, cJSON *data)
{
  ipc_broadcast(app->ipc, event, data);
}

void app_emit_clip(struct app *app, const char *event, int64_t id)
{
  cJSON *row = db_row(app->db, id);
  if (row) app_emit(app, event, row);
}

void app_emit_state(struct app *app)
{
  cJSON *d = cJSON_CreateObject();
  cJSON_AddBoolToObject(d, "paused", app_paused(app));
  cJSON_AddNumberToObject(d, "paused_until", (double)setting_int(app->db, "paused_until"));
  app_emit(app, "state.changed", d);
}

static void put_bool(struct app *app, const char *key, bool v)
{
  cJSON *b = cJSON_CreateBool(v);
  const char *err;
  setting_set(app->db, key, b, &err);
  cJSON_Delete(b);
}

static void put_int(struct app *app, const char *key, int64_t v)
{
  cJSON *n = cJSON_CreateNumber((double)v);
  const char *err;
  setting_set(app->db, key, n, &err);
  cJSON_Delete(n);
}

bool app_paused(struct app *app)
{
  if (!setting_bool(app->db, "paused")) return false;
  int64_t until = setting_int(app->db, "paused_until");
  return !until || now_ms() < until;
}

static void on_pause_expired(void *ctx)
{
  struct app *app = ctx;
  app->pause_timer = NULL;
  if (setting_bool(app->db, "paused") && setting_int(app->db, "paused_until")) {
    log_info("timed pause over; capturing again");
    app_resume(app);
  }
}

void app_pause(struct app *app, int minutes)
{
  loop_timer_cancel(app->loop, app->pause_timer);
  app->pause_timer = NULL;
  int64_t until = minutes > 0 ? now_ms() + (int64_t)minutes * 60000 : 0;
  put_bool(app, "paused", true);
  put_int(app, "paused_until", until);
  if (until) app->pause_timer = loop_timer(app->loop, until - now_ms(), on_pause_expired, app);
  log_info("capture paused%s", until ? " (timed)" : "");
  app_emit_state(app);
}

void app_resume(struct app *app)
{
  loop_timer_cancel(app->loop, app->pause_timer);
  app->pause_timer = NULL;
  put_bool(app, "paused", false);
  put_int(app, "paused_until", 0);
  log_info("capture resumed");
  app_emit_state(app);
}

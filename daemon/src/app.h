/*
 * The daemon's shared state. One instance, owned by main().
 *
 * Module map:
 *   wl.c       Wayland connection, data-control device, offers
 *   capture.c  turning a new selection into a stored clip
 *   serve.c    owning the selection and streaming payloads to readers
 *   paste.c    "paste this into the previous window": serve + keystroke
 *   hypr.c     Hyprland request socket, event socket, Lua eval
 *   ipc.c      the JSON-lines socket for the UI and the CLI
 *   proto.c    the operations that socket exposes
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct cJSON cJSON;
struct loop;
struct loop_timer;
struct db;
struct wl;
struct hypr;
struct ipc;
struct capture;
struct serve;
struct hotkeys;

struct app {
  struct loop *loop;
  struct db *db;
  struct wl *wl;         /* NULL with --no-wayland */
  struct hypr *hypr;     /* NULL outside Hyprland */
  struct ipc *ipc;
  struct capture *capture;
  struct serve *serve;
  struct hotkeys *hotkeys;

  char *runtime_dir;     /* $XDG_RUNTIME_DIR/clipnet */
  char *socket_path;

  struct loop_timer *retention_timer;
  struct loop_timer *pause_timer;
};

/* Broadcast an event to subscribed IPC clients. Takes ownership of data. */
void app_emit(struct app *app, const char *event, cJSON *data);
/* Emit clip.added / clip.updated with the row for id. */
void app_emit_clip(struct app *app, const char *event, int64_t id);
void app_emit_state(struct app *app);

bool app_paused(struct app *app);
/* minutes <= 0: until resumed. */
void app_pause(struct app *app, int minutes);
void app_resume(struct app *app);

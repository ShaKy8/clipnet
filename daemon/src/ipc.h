/*
 * The control socket: JSON lines over a Unix socket in a 0700 directory,
 * accepting only our own uid. docs/PROTOCOL.md is the reference.
 *
 *   request  {"id": N, "op": "list", ...}
 *   reply    {"id": N, "ok": true, "result": ...}
 *            {"id": N, "ok": false, "error": {"code": "...", "message": "..."}}
 *   event    {"event": "clip.added", "data": {...}}   (only after hello+subscribe)
 */
#pragma once

#include <stdbool.h>

typedef struct cJSON cJSON;
struct app;
struct ipc;
struct ipc_client;

struct ipc *ipc_listen(struct app *app, const char *path);
void ipc_close(struct ipc *ipc);

/* Both take ownership of result. id may be NULL (no id in the request). */
void ipc_reply(struct ipc_client *c, const cJSON *id, cJSON *result);
void ipc_error(struct ipc_client *c, const cJSON *id, const char *code, const char *message);
void ipc_subscribe(struct ipc_client *c, bool on);
/* Send to every subscribed client. Takes ownership of data. */
void ipc_broadcast(struct ipc *ipc, const char *event, cJSON *data);

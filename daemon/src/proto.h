#pragma once

typedef struct cJSON cJSON;
struct app;
struct ipc_client;

#define CLIPNET_VERSION "1.0.0"
#define CLIPNET_PROTO 1

/* Handle one request; always sends exactly one reply to c. */
void proto_handle(struct app *app, struct ipc_client *c, const cJSON *req);

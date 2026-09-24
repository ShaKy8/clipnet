/*
 * Copy buffers: Ditto's extra clipboards. Copying into buffer N and pasting
 * from it leave the normal clipboard as it was.
 *
 *   copy N   press Ctrl+C (Ctrl+Insert in terminals) in the focused window,
 *            keep what arrives as buffer N, put the previous clip back
 *   cut N    the same with Ctrl+X
 *   paste N  serve buffer N, press paste, and once the app has read it,
 *            put the previous clip back
 *
 * Buffered clips are ordinary history clips (the copy is recorded like any
 * other); the slot just points at one, and retention never removes it.
 */
#pragma once

#include <stdbool.h>

typedef struct cJSON cJSON;
struct app;
struct buffers;

#define BUFFER_SLOTS 3

struct buffers *buffers_new(struct app *app);
void buffers_free(struct buffers *b);

int buffers_copy(struct buffers *b, int slot, bool cut, const char **err);
int buffers_paste(struct buffers *b, int slot, const char **err);
/* [{slot, clip: row | null}] */
cJSON *buffers_state(struct buffers *b);

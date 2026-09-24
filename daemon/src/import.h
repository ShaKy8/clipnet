/* One-time import of other clipboard histories. */
#pragma once

typedef struct cJSON cJSON;
struct db;

/* Omarchy's clipboard plugin history: a JSON array, newest first, of
 * {"type":"text","text":...} and {"type":"image","mime":...,"path":...}.
 * Image files are copied into our blob store, never moved or changed.
 * Returns {"added": N, "duplicates": N, "skipped": N}, or NULL with *err set. */
cJSON *import_omarchy(struct db *db, const char *path, const char **err);

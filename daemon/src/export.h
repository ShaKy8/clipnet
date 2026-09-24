/*
 * Portable export and import: one JSON file holding groups and clips with
 * every format (payloads base64). Meant for backups and for moving history
 * to another machine, including the future macOS app; see docs/EXPORT.md.
 */
#pragma once

#include <stdint.h>

typedef struct cJSON cJSON;
struct db;

/* group_id 0: everything. Returns {"clips": N, "groups": N, "path": ...}. */
cJSON *export_json(struct db *db, const char *path, int64_t group_id, const char **err);
/* Clips whose uuid (or content) is already present are skipped.
 * Returns {"added": N, "duplicates": N, "skipped": N, "groups": N}. */
cJSON *import_json(struct db *db, const char *path, const char **err);

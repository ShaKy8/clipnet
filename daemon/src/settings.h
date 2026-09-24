/*
 * User settings: JSON values in the settings table, with defaults here.
 * Unknown keys are rejected so a typo in the UI or CLI is an error, not a
 * setting that silently does nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;
struct db;

enum setting_type { SET_BOOL, SET_INT, SET_STRING };

struct setting_def {
  const char *key;
  enum setting_type type;
  const char *def;      /* default, as JSON text */
  int64_t min, max;     /* SET_INT bounds */
  const char *choices;  /* SET_STRING: "|a|b|c|" or NULL for free text */
};

const struct setting_def *setting_find(const char *key);
const struct setting_def *settings_all(size_t *n);

bool setting_bool(struct db *db, const char *key);
int64_t setting_int(struct db *db, const char *key);
/* Caller frees. */
char *setting_string(struct db *db, const char *key);

/* Validate and store; on error returns -1 and sets *err to a static string. */
int setting_set(struct db *db, const char *key, const cJSON *value, const char **err);
/* Every setting with its current value. */
cJSON *settings_dump(struct db *db);

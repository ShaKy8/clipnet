#pragma once

typedef struct sqlite3 sqlite3;

int schema_version_latest(void);
/* Bring the database up to the latest schema. 0 on success. */
int schema_migrate(sqlite3 *db);

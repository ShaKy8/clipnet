/*
 * Schema and migrations. docs/SCHEMA.md describes the same thing for humans
 * (and for the future macOS app, which must be able to open this file).
 *
 * Portability rules: times are Unix milliseconds UTC, text is UTF-8, formats
 * are MIME names, every user-visible row has a uuid. Nothing here names a
 * compositor; the one platform hint is clip_formats.platform.
 */
#include "schema.h"

#include <sqlite3.h>
#include <stdlib.h>

#include "util.h"

static const char *const v1 =
  "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;"

  "CREATE TABLE groups("
  "  id INTEGER PRIMARY KEY,"
  "  uuid TEXT UNIQUE NOT NULL,"
  "  parent_id INTEGER REFERENCES groups(id) ON DELETE CASCADE,"
  "  name TEXT NOT NULL,"
  "  sort_order REAL NOT NULL DEFAULT 0,"
  "  created_at INTEGER NOT NULL);"
  /* UNIQUE(parent_id, name) would let duplicate top-level names through,
   * since NULLs never compare equal; an expression index closes that gap. */
  "CREATE UNIQUE INDEX groups_name ON groups(ifnull(parent_id, 0), name);"

  "CREATE TABLE clips("
  "  id INTEGER PRIMARY KEY,"
  "  uuid TEXT UNIQUE NOT NULL,"
  "  created_at INTEGER NOT NULL,"
  "  last_used_at INTEGER NOT NULL,"
  "  paste_count INTEGER NOT NULL DEFAULT 0,"
  "  kind TEXT NOT NULL CHECK(kind IN ('text','rich','image','files','other')),"
  "  title TEXT,"
  "  preview TEXT NOT NULL,"
  "  plain_text TEXT,"
  "  content_hash BLOB NOT NULL,"
  "  total_size INTEGER NOT NULL,"
  "  source_app TEXT,"
  "  source_title TEXT,"
  "  group_id INTEGER REFERENCES groups(id) ON DELETE SET NULL,"
  "  sticky_order REAL,"
  "  locked INTEGER NOT NULL DEFAULT 0,"
  "  quick_paste TEXT UNIQUE,"
  "  flags INTEGER NOT NULL DEFAULT 0);"
  "CREATE INDEX clips_recent ON clips(last_used_at DESC);"
  "CREATE INDEX clips_group ON clips(group_id, sticky_order, last_used_at DESC);"
  "CREATE UNIQUE INDEX clips_hash ON clips(content_hash);"

  "CREATE TABLE clip_formats("
  "  clip_id INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,"
  "  ord INTEGER NOT NULL,"
  "  mime TEXT NOT NULL,"
  "  raw_name TEXT NOT NULL,"
  "  aliases TEXT,"
  "  platform TEXT,"
  "  size INTEGER NOT NULL,"
  "  hash BLOB NOT NULL,"
  "  data BLOB,"
  "  blob TEXT,"
  "  PRIMARY KEY(clip_id, mime)) WITHOUT ROWID;"
  "CREATE INDEX formats_blob ON clip_formats(blob) WHERE blob IS NOT NULL;"

  "CREATE TABLE hotkeys("
  "  id INTEGER PRIMARY KEY,"
  "  accel TEXT UNIQUE NOT NULL,"
  "  action TEXT NOT NULL,"
  "  arg TEXT,"
  "  enabled INTEGER NOT NULL DEFAULT 1);"

  "CREATE TABLE copy_buffers("
  "  slot INTEGER PRIMARY KEY,"
  "  clip_id INTEGER REFERENCES clips(id) ON DELETE SET NULL);"

  "CREATE TABLE rules("
  "  id INTEGER PRIMARY KEY,"
  "  ord REAL NOT NULL,"
  "  enabled INTEGER NOT NULL DEFAULT 1,"
  "  match_app TEXT,"
  "  match_mime TEXT,"
  "  action TEXT NOT NULL,"
  "  arg TEXT);"

  "CREATE TABLE scripts("
  "  id INTEGER PRIMARY KEY,"
  "  path TEXT UNIQUE NOT NULL,"
  "  hook TEXT NOT NULL CHECK(hook IN ('on_copy','on_paste')),"
  "  enabled INTEGER NOT NULL DEFAULT 0,"
  "  ord REAL NOT NULL);"

  "CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"

  "CREATE VIRTUAL TABLE clips_fts USING fts5("
  "  title, plain_text, source_app,"
  "  content='clips', content_rowid='id', tokenize='trigram');"
  "CREATE TRIGGER clips_fts_ai AFTER INSERT ON clips BEGIN"
  "  INSERT INTO clips_fts(rowid, title, plain_text, source_app)"
  "  VALUES (new.id, new.title, new.plain_text, new.source_app);"
  "END;"
  "CREATE TRIGGER clips_fts_ad AFTER DELETE ON clips BEGIN"
  "  INSERT INTO clips_fts(clips_fts, rowid, title, plain_text, source_app)"
  "  VALUES ('delete', old.id, old.title, old.plain_text, old.source_app);"
  "END;"
  "CREATE TRIGGER clips_fts_au AFTER UPDATE OF title, plain_text, source_app ON clips BEGIN"
  "  INSERT INTO clips_fts(clips_fts, rowid, title, plain_text, source_app)"
  "  VALUES ('delete', old.id, old.title, old.plain_text, old.source_app);"
  "  INSERT INTO clips_fts(rowid, title, plain_text, source_app)"
  "  VALUES (new.id, new.title, new.plain_text, new.source_app);"
  "END;"

  /* Seeded exclusions, as case-insensitive globs over the focused window's
   * class, for password managers that may not set the KDE hint. 1Password's
   * desktop file declares com.onepassword.OnePassword; its native Wayland
   * app id has been seen as plain "1Password", so both are covered. */
  "INSERT INTO rules(ord, match_app, action) VALUES"
  "  (1, '*keepassxc*', 'exclude'),"
  "  (2, '*1password*', 'exclude'),"
  "  (3, '*onepassword*', 'exclude'),"
  "  (4, '*bitwarden*', 'exclude');";

/* Each entry upgrades from version i to i+1. Append only; never edit. */
static const char *const migrations[] = { v1 };

int schema_version_latest(void)
{
  return (int)ARRAY_LEN(migrations);
}

static int user_version(sqlite3 *db)
{
  sqlite3_stmt *st;
  int v = -1;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  return v;
}

int schema_migrate(sqlite3 *db)
{
  int v = user_version(db);
  if (v < 0) return -1;
  if (v > schema_version_latest()) {
    log_err("database schema v%d is newer than this build (v%d)", v, schema_version_latest());
    return -1;
  }
  for (; v < schema_version_latest(); v++) {
    char *err = NULL;
    char *sql = xasprintf("BEGIN IMMEDIATE; %s; PRAGMA user_version = %d; COMMIT;", migrations[v], v + 1);
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    free(sql);
    if (rc != SQLITE_OK) {
      log_err("migration to v%d failed: %s", v + 1, err ? err : "?");
      sqlite3_free(err);
      sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      return -1;
    }
    log_info("database migrated to schema v%d", v + 1);
  }
  return 0;
}

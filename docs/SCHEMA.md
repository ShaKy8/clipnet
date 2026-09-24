# The CLIP//NET database (schema v3)

`~/.local/share/clipnet/clipnet.db` is SQLite in WAL mode, with large payloads
stored beside it in `blobs/`. Only the daemon writes to it. This document is
the contract a future macOS daemon has to honour to share a history, or an
export, with the Linux one. The authoritative DDL is `daemon/src/schema.c`,
which migrates in order; `PRAGMA user_version` holds the version.

## Conventions

- Times are Unix **milliseconds**, UTC (`created_at`, `last_used_at`).
- Text is UTF-8. Every clip and group has a random v4 `uuid`, which is what
  exports, sync and hotkeys refer to. Local integer ids never leave a machine.
- Formats are named by **MIME type**. The canonical text format is
  `text/plain;charset=utf-8`, and its bytes are always UTF-8.

## Tables

| Table | Purpose |
|---|---|
| `clips` | One row per clip: `kind` (`text`/`rich`/`image`/`files`/`other`), `preview` (one line, ≤300 chars), `plain_text` (searchable text, ≤1M chars), `content_hash` (SHA-256 over the sorted `(mime, length, bytes)` of every format; the dedupe key), `total_size`, `title`, `group_id`, `sticky_order` (NULL = not sticky; ascending = top first), `locked`, `quick_paste` (unique), `paste_count`, `source_app`, `source_title`, `flags` |
| `clip_formats` | One row per format of a clip: `mime`, `ord` (offer order), `size`, `hash` (SHA-256 of the bytes), and either `data` (inline, ≤64 KiB and not an image) or `blob` (`ab/<sha256 hex>` under `blobs/`) |
| `clips_fts` | FTS5 (trigram tokenizer) over `title`, `plain_text`, `source_app`; kept in step by triggers |
| `groups` | Nested groups: `parent_id`, `name` (unique among siblings, top level included), `sort_order` |
| `hotkeys` | `accel` in the portable spelling (`Ctrl+Alt+1`), `action`, `arg` (a clip **uuid**, a position, a buffer slot) |
| `copy_buffers` | `slot` 1–3 → `clip_id` |
| `rules` | `action` (`exclude`, `skip_mime`, `paste_keys`, `to_group`), `match_app` and `match_mime` globs, `arg` JSON (`{"keys": "Ctrl+Shift+V"}`, `{"group": uuid}`), `ord` |
| `scripts` | `name` (a file in the scripts directory) → `enabled` |
| `settings` | `key` → JSON value; defaults and validation live in `daemon/src/settings.c` |
| `meta` | reserved |

Indexes that matter for speed: `clips_order` and `clips_group_order` follow
the list order exactly (sticky first, then most recently used), and the
`clips_hash` unique index backs dedupe.

## What is platform-specific, and how another platform treats it

| Field | Linux value | On another platform |
|---|---|---|
| `clip_formats.aliases` | The other names the source offered text under, including X11 names like `UTF8_STRING` and `STRING` | Informational: offer the canonical MIME, map it to the native type, ignore aliases it doesn't know |
| `clip_formats.raw_name` | The name actually read | Informational |
| `clip_formats.platform` | always NULL (portable) | Set it for a format that only makes sense natively; readers skip formats for another platform |
| `clips.source_app`, `rules.match_app` | Hyprland window class (`com.mitchellh.ghostty`) | The app's identifier there (a bundle id); rules are per-machine in practice |
| `settings.capture_primary` | PRIMARY selection capture | No equivalent on macOS; ignore |
| `hotkeys.accel` | `Super` = the Super/Windows key | Map `Super` → ⌘ and `Alt` → ⌥ |

MIME ↔ macOS pasteboard types a Mac daemon would map: `text/plain;charset=utf-8`
↔ `public.utf8-plain-text`, `text/html` ↔ `public.html`, `text/rtf` ↔
`public.rtf`, `image/png` ↔ `public.png`, `image/tiff` ↔ `public.tiff`,
`text/uri-list` ↔ `public.file-url` (one per item).

The socket protocol (`docs/PROTOCOL.md`) has no Wayland or Hyprland concepts in
its shape. Only `show_context`'s monitor geometry and the hotkey conflict
messages carry compositor details, and both are plain data.

## Blobs

`blobs/ab/abcdef…` holds the bytes of a format whose SHA-256 is `abcdef…`.
Identical content is stored once, whichever clip or format it belongs to. A
blob file is deleted when no `clip_formats` row references it any more (at
delete time, and hourly by retention's garbage collection). Files are 0600,
directories 0700.

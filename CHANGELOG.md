# Changelog

All notable changes to CLIP//NET are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Database schema
changes migrate automatically; the schema version is noted when it changes.

## [Unreleased]

Project infrastructure only; the program is unchanged since 1.0.0.

### Added

- Continuous integration on GitHub Actions: `make test` and `make asan`, plus
  the live clipboard test against a headless Sway, in an Arch Linux container.
- `CONTRIBUTING.md`, `SECURITY.md` (with private vulnerability reporting
  switched on) and `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1).
- Issue forms (bug report, feature request, private contact request) and a
  pull request template.

## [1.0.0] - 2026-09-24

The first release: [Ditto](https://github.com/sabrogden/Ditto)'s clipboard
history for Omarchy on Hyprland. Database schema v3.

### Added

- **Capture of every format** of every copy (text under all its aliases, HTML,
  rich text, images, file lists, app-specific formats), stored in SQLite with
  large payloads in content-addressed files, and pasted back byte for byte.
- **The popup**, opened with Ctrl+' or Super+Ctrl+V at the mouse cursor and
  styled from the current Omarchy theme, following theme and font changes.
  - Search as you type (any substring; every word must match).
  - Enter pastes into the previous window, Shift+Enter pastes plain text,
    Ctrl+1…0 pastes the first ten clips.
  - Multi-select paste, F3 full view, hover preview.
  - Ctrl+Space keeps the popup open after a paste; Ctrl+F2 compares two clips.
- **Organising:** nested groups (Ctrl+G, F7, Ctrl+F7), sticky clips,
  never-delete locks, descriptions, quick-paste words, a clip editor, new
  clips, a properties dialog and a right-click menu.
- **Special Paste:** UPPER, lower, Title, Sentence and inverted case,
  camelCase, snake_case, slugify, trim, remove or add line breaks, date and time.
- **Global hotkeys:** a hotkey per clip, history-position hotkeys, pause/resume,
  and three copy buffers (copy, cut and paste each). Keys already bound in
  Hyprland are refused with the reason.
- **Rules:** ignore apps (KeePassXC, 1Password and Bitwarden by default), drop
  formats, per-app paste keys, and route an app's copies into a group.
  Copies marked with a password-manager hint are never stored.
- **Lua scripting:** `on_copy` / `on_paste` hooks in a sandbox (no file,
  process or network access; 64 MiB and 100 ms per call), with a dry-run
  tester and four examples. See `docs/SCRIPTING.md`.
- **Keep-alive:** the last clip survives the app you copied from quitting, and
  the newest clip returns after a restart if the clipboard is empty.
- Optional capture of the PRIMARY (select-to-copy) selection.
- **Settings window:** General, Capture, History, Paste, Buffers, Rules,
  Scripts, Hotkeys and Data pages.
- **Retention:** unlimited by default, with large clips expiring after 30 days;
  sticky, grouped, locked and hotkeyed clips are never removed automatically.
- **Data:** import of Omarchy's clipboard history; portable JSON export and
  import; database compaction.
- **`scripts/clipnet`:** start/stop/status, paste and copy by position, list,
  pause, settings, and a reversible `install` / `uninstall`. The installer
  replaces Omarchy's clipboard plugin, edits only marked blocks in
  `~/.config/hypr`, and installs theme and font hooks.
- Documentation: `README.md`, `docs/PROTOCOL.md`, `docs/SCHEMA.md` and
  `docs/SCRIPTING.md`. MIT license.

[Unreleased]: https://github.com/ShaKy8/clipnet/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/ShaKy8/clipnet/releases/tag/v1.0.0

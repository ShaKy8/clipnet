# CLIP//NET

[Ditto](https://github.com/sabrogden/Ditto) for Omarchy: every copy is kept
with all of its formats, and **Ctrl+'** brings the history up at the mouse
cursor. Type to search, then press Enter to paste into the window you came from.

```
scripts/clipnet install      daemon as a user service, Ctrl+' and Super+Ctrl+V,
                             replaces Omarchy's clipboard manager, imports its history
scripts/clipnet uninstall    undo all of that (history kept; --purge deletes it)
scripts/clipnet              start (no-op if running)      stop | restart | status
scripts/clipnet list [TEXT]  recent clips / search         paste N | copy N
scripts/clipnet pause [MIN]  stop recording (for MIN minutes); resume
scripts/clipnet set KEY VAL  change a setting (see `clipnet settings`)
```

## In the popup

| Key | | Key | |
|---|---|---|---|
| type | search (substring, every word must match) | Enter / double-click | paste |
| Shift+Enter | paste as plain text | Ctrl+1 … Ctrl+0 | paste row 1–10 |
| ↑ ↓ PgUp PgDn, Ctrl+Home/End | move | Shift+↑↓, Ctrl/Shift+click, Ctrl+A | select several, then Enter pastes them joined |
| F3 | view the whole clip (Alt+W wraps; Shift+PgUp/PgDn scrolls) | Delete | delete the clip(s) |
| Ctrl+C | copy to the clipboard without pasting | Alt+C | clear the search |
| Esc | clear the search, then close | Ctrl+' | close |

Ctrl+1–0, Shift+Enter, F3, Alt+C and Ctrl+C are Ditto's own defaults (from
its `ActionEnums.cpp`). The rest follow the usual list conventions.

## How it works

```
Hyprland ── Ctrl+' → hl.dsp.global("clipnet:toggle") ──► quickshell (this dir): the popup
   ▲  ▲                                                        │ JSON lines
   │  └── socket2 events (focused app, window closed) ──►  clipnetd (daemon/, C)
   └──── Lua eval: paste keystroke ◄──────────────────────     │ ext-data-control-v1
                                                               ▼
                               ~/.local/share/clipnet/{clipnet.db, blobs/}
```

- **clipnetd** watches the clipboard through `ext-data-control-v1` and reads
  *every* format each copy offers: plain text under all its aliases, HTML,
  RTF, images, file lists, and app-private types. Text is read once under its
  best name and offered back under every name the source used. It stores
  everything in SQLite (full-text search through FTS5 trigrams, so any
  substring matches). Images and anything over 64 KB go in content-addressed
  files. The daemon runs as `clipnetd.service`: it owns the clipboard while a
  pasted clip is on it, so it must outlive UI restarts.
- **The popup** is a Quickshell overlay. It talks only to the daemon, never
  to the database, and follows the Omarchy theme live: colors from
  `colors.toml` and the `[menu]` tokens in `shell.toml`, fonts from `[font]`,
  and corner rounding from Hyprland.
- **Pasting** puts the clip on the clipboard, then hands Hyprland one Lua
  chunk. The chunk waits until the popup's layer is gone *and* no modifier
  key is held (so Ctrl+1 doesn't become Ctrl+Ctrl+V), then presses Ctrl+V,
  or Shift+Insert in windows tagged `terminal`. That split press/release is
  Omarchy's own trick for stuck synthetic keys.
- **Keep-alive**: when the app you copied from quits, its clipboard contents
  would go with it. Hyprland doesn't announce that to clipboard managers, so
  when a window closes, focus moves, or the popup opens, the daemon checks
  whether the clipboard is empty. If it is, the daemon puts back the last clip
  it captured. It never does this after a password manager cleared the
  clipboard on purpose.

## Privacy

- Nothing leaves the machine. Files are private (0600/0700), deleted clips are
  zeroed in the database, and logs never contain clip contents.
- Copies marked with `x-kde-passwordManagerHint` (what KeePassXC and others
  send) are never stored.
- Copies made while KeePassXC, 1Password or Bitwarden has focus are excluded
  by default rules. Those rules match the *focused* window's class, so a
  password copied from a browser extension is attributed to the browser. Use
  `clipnet pause` if you rely on an extension.

## Development

```
make test        unit tests (C) and config-block tests
make test-live   captures and serves through the real clipboard on a throwaway
                 database; saves and restores your clipboard
make asan        unit tests under AddressSanitizer + UBSan
CLIPNETD=/path/to/asan/clipnetd tests/integration/live_test.sh
```

`docs/PROTOCOL.md` is the socket API. The schema lives in
`daemon/src/schema.c` and uses only portable types: ms timestamps, MIME
names, uuids.

## Notes from the spikes (Hyprland 0.56.2, Quickshell 0.3.1)

- Hyprland exports `ext_data_control_manager_v1` v1. Serving seven types from
  one source works, and `text/html` comes back byte-exact.
- The raw request socket takes `eval <lua>` and `repl <lua>` directly, in
  about 0.1 ms per request, with no `hyprctl` fork.
- Hyprland sends **no** `selection(NULL)` to data-control clients when the
  clipboard owner exits. A newly created data device does receive the current
  selection, so keep-alive probes with a throwaway device plus a
  `wl_display_sync`. The offers must outlive that sync: libwayland passes an
  already-destroyed offer to the `selection` event as NULL.
- Once the popup hides, `hl.get_layers({namespace="clipnet"})` is empty within
  50 ms, and `hl.get_active_window()` is the window the user came from.

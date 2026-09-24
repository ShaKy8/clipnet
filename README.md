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
scripts/clipnet settings     the settings window         (or set KEY VAL from a terminal)
```

## In the popup

| Key | | Key | |
|---|---|---|---|
| type | search (substring, every word must match) | Enter / double-click | paste (on a group: open it) |
| Shift+Enter | paste as plain text | Ctrl+1 … Ctrl+0 | paste clip 1–10 |
| Ctrl+Shift+V | Special Paste: case, trim, one line, camelCase, slug, date… | Menu / Shift+F10 / right-click | everything else |
| ↑ ↓ PgUp PgDn, Ctrl+Home/End | move | Shift+↑↓, Ctrl/Shift+click, Ctrl+A | select several; Enter pastes them joined |
| F3 | view the whole clip (Alt+W wraps; Shift+PgUp/PgDn scrolls) | Delete | delete the clip(s), or the group |
| Ctrl+E / Ctrl+N | edit the clip / write a new one | Alt+Enter | properties: description, group, hotkey, quick-paste word |
| Ctrl+S / Ctrl+L | sticky (pin to top) / never auto-delete | Alt+↑↓ | reorder sticky clips |
| Ctrl+G | Groups ↔ History | Backspace (empty search) | up one level |
| F7 / Ctrl+F7 | new group from the selection / empty group | F2 | rename the group |
| Ctrl+C | copy without pasting | Alt+C | clear the search |
| Ctrl+Space | keep the popup open after pasting | Ctrl+F2 | compare two selected clips side by side |
| Ctrl+, | settings | Esc | clear the search, then close |

These are Ditto's own defaults, taken from its `ActionEnums.cpp`: Enter,
Shift+Enter, Ctrl+1–0, F3, Ctrl+G, F7/Ctrl+F7, Ctrl+N, Ctrl+E, Alt+Enter, Alt+C,
Ctrl+C, Ctrl+Space, Ctrl+F2 and Delete. The rest are CLIP//NET's own. Ditto's
Ctrl+F2 opens an external diff tool; CLIP//NET's comparison is built in.
Resting the mouse on a row shows more of the clip.

A **quick-paste word** (set in Properties) works like Ditto's: type the word,
its clip jumps to the top, and Enter pastes it. A clip's **hotkey** pastes it
from anywhere without opening the popup. The Settings window (Hotkeys page)
can also bind history positions, e.g. Ctrl+Alt+1 for "the newest clip".
Hotkeys that clash with an existing Hyprland binding are refused, and the
message says what already owns them.

## Beyond the popup

- **Copy buffers** (Settings → Buffers): three extra clipboards, as in Ditto.
  Give each a *copy into*, *cut into* and *paste* hotkey. Copying into a
  buffer, or pasting from one, leaves your normal clipboard as it was.
- **Rules** (Settings → Rules): ignore an app entirely, drop a format
  (say `image/*` from a browser), paste with a different key in an app (for
  example Ctrl+Shift+V in a terminal that lacks Omarchy's Shift+Insert
  mapping), or file an app's copies straight into a group.
- **Pause** recording for 15 minutes, an hour or four (Settings → Capture,
  `clipnet pause 60`, or a hotkey). The popup shows the time left.
- **Select-to-copy** (Linux's PRIMARY selection) can be recorded too. It's
  off by default, because every highlight would otherwise land in the history.
- **Scripts** (Settings → Scripts): Lua scripts that change or skip clips as
  they're copied, or change text as it's pasted. For example: strip tracking
  parameters from links, skip one-time codes, file git URLs into a group, or
  keep pasted commands from running in a terminal. Examples are included,
  scripts run in a sandbox, and each starts off. See `docs/SCRIPTING.md`.

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

## How it stays cheap

Measured on this machine with Hyprland 0.56 and SQLite 3.53:

- **Idle:** the daemon sleeps in `epoll_wait`, with no timers apart from an
  hourly retention pass. It wakes only to read Hyprland's event stream, and it
  only acts on a real focus change. About 6 MB of memory.
- **Opening the popup:** the popup is always loaded, so opening it is a
  visibility flip plus one request for the cursor position and the first
  300 rows. That request takes 3 ms with 50,000 clips, because an index
  matches the list order exactly (sticky first, then most recent).
- **Search, at 50,000 clips (`make -C daemon bench`):** a rare term takes
  under 1 ms. A word found in half the history takes about 18 ms: the daemon
  first probes how common a term is, then either sorts the few matches or
  walks the history in order and stops at a page's worth. One- or
  two-character searches can't use the trigram index and take about 60 ms;
  they only happen for a moment while you type.
- **Capture:** about 0.3 ms per clip plus the time to read it from the app.
  Formats are read one at a time without blocking the daemon, and pastes of
  any size are streamed.

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
make test        unit (C and QML), config-block and IPC tests; no session needed
make test-live   captures and serves through the real clipboard on a throwaway
                 database; saves and restores your clipboard, and pauses your
                 own clipnetd meanwhile so test copies never reach your history
make asan        unit tests under AddressSanitizer + UBSan
make -C daemon bench   timings with 50,000 synthetic clips (N=… to change)
CLIPNETD=/path/to/asan/clipnetd tests/integration/live_test.sh
```

`docs/PROTOCOL.md` is the socket API, `docs/SCHEMA.md` is the database
contract (including what a macOS port maps differently), and exports are self-describing JSON
(`"format": "clipnet-export"`), with every format of every clip base64-encoded. The schema lives in
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

## License

MIT (see `LICENSE`). Bundled third-party files keep their own licenses, stated
at the top of each: `daemon/vendor/cJSON.{c,h}` (MIT) and
`daemon/protocols/hyprland-global-shortcuts-v1.xml` (BSD-3-Clause).
CLIP//NET recreates Ditto's behaviour but contains none of its code.

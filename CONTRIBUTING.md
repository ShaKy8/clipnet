# Contributing to CLIP//NET

Thanks for helping. CLIP//NET is a small project with a clear goal: bring
[Ditto](https://github.com/sabrogden/Ditto)'s clipboard history to Omarchy and
Hyprland, faithfully and without fuss. Bug reports, fixes, and features that
close a gap with Ditto are all welcome. For a larger change, open an issue
first so we can agree on the approach before you spend the time.

## Reporting a bug

Please include:

- What you did, what you expected, and what happened instead.
- The output of `scripts/clipnet status`.
- Your Omarchy, Hyprland (`hyprctl version | head -1`) and Quickshell
  (`quickshell --version`) versions.
- The daemon's log from around the time: `journalctl --user -u clipnetd --since "10 min ago"`.
  CLIP//NET never logs clip contents, but your own scripts' `clipnet.log()`
  lines appear there too, so read the log before you paste it.
- For popup problems, the UI log: `quickshell log -p <your checkout>`.

**Never attach your clipboard database**, `~/.local/share/clipnet/`. It is your
entire copy history.

**Security issues** (anything that could leak clipboard contents, or let
another user or process reach the daemon): don't describe them in a public
issue. Report them privately with GitHub's
[Report a vulnerability](https://github.com/ShaKy8/clipnet/security/advisories/new)
form, on the repository's **Security** tab. Only the maintainers see the report.

## Building and testing

On Omarchy everything needed is already installed. Elsewhere you need gcc,
make, pkg-config, the wayland, wayland-protocols (1.39+), sqlite3 and lua 5.4
development files, Quickshell 0.3+, jq and python3.

```
make                 # daemon/clipnetd
make test            # C unit, QML, IPC and config-block tests; no desktop needed
make asan            # the unit tests under AddressSanitizer + UBSan
make test-live       # the real clipboard round trip (needs a Wayland session)
make -C daemon bench # timings with 50,000 synthetic clips
```

A single C test file: `make -C daemon build/tests/test_db && (cd daemon && build/tests/test_db)`.
Set `CLIPNET_TEST_LOG=1` to see the daemon's log output while it runs.

To try the UI against a scratch daemon instead of your real history:

```
daemon/clipnetd --no-wayland --data /tmp/scratch --socket $XDG_RUNTIME_DIR/scratch.sock &
pkill -f "quickshell.* -p $PWD\$"                           # stop only the UI; the daemon keeps your clipboard
CLIPNET_SOCKET=$XDG_RUNTIME_DIR/scratch.sock quickshell -p "$PWD"
scripts/clipnet restart-ui                                  # afterwards: back to your real history
```

**Tests must never touch the user's real history or config.** The existing
suites use throwaway data, config and socket directories, and the live test
pauses a running `clipnetd` while it copies. Keep new tests the same way.

CI (GitHub Actions) runs `make test` and `make asan`, plus the live test against
a headless Sway, in an Arch Linux container. A pull request needs all three
green.

## How the code fits together

`README.md` has the overview and the diagram. In short: `daemon/` is a
single-threaded C daemon (Wayland clipboard, SQLite, Hyprland IPC, Lua), and the
Quickshell UI (`shell.qml`, `components/`, `settings/`, `common/`) talks to it
only through the JSON-lines socket in `docs/PROTOCOL.md`. Each `daemon/src/*.h`
opens with a comment on what the module owns; start there.

## Conventions

**C** (`daemon/src`): C11, two-space indent, braces on the function's own line
and cuddled everywhere else, lines up to about 120 characters. Everything the
daemon does happens on its event loop, so nothing may block: no `sleep`, and
no reading a pipe or socket until it's ready. Allocate with `xmalloc` and
friends (from `util.h`), which abort on out-of-memory. Every new public
function gets a comment in its header.

**QML / JS**: two-space indent, `Theme.*` for every colour, size and font (so it
follows the Omarchy theme), and `Daemon.call(op, args, cb)` for everything
the UI asks of the daemon. Pure logic goes in a `.pragma library` `.js` file
with a test in `tests/qml/`.

**Comments** say *why*, not *what*. Match the tone of the code around them.

**Things that must stay true:**

- **Clip contents never reach a log.** Log ids, MIME types, sizes, app classes
  and timings, never the text or bytes.
- **Schema migrations are append-only.** Add a new entry to `migrations[]` in
  `daemon/src/schema.c` and never edit a released one. Update
  `docs/SCHEMA.md` with it.
- **The protocol stays portable** (no Wayland or Hyprland concepts in its
  shape). Every new operation or event is documented in `docs/PROTOCOL.md`
  and covered by `tests/integration/ipc_test.py`.
- **Nothing is written under `/usr/share`**, and every change to the user's
  config lives in a `CLIP//NET (managed)` block that `clipnet uninstall`
  removes.
- **Files are private:** 0600 for files and 0700 for directories.
- **Vendored files stay as upstream shipped them:** `daemon/vendor/` (cJSON)
  and `daemon/protocols/` (Hyprland's protocol XML).

## Pull requests

- Keep each one to a single change, with tests that fail without it.
- Explain the *why* in the description. For UI changes, include a screenshot,
  taken with demo data (`tools/readme-screenshot.sh` shows how), never your
  real history.
- Update the docs your change touches: `README.md` for keys and features,
  `docs/SCRIPTING.md` for script API changes, `docs/PROTOCOL.md` and
  `docs/SCHEMA.md` as above.
- Commit messages: a short summary line, a blank line, then what changed and
  why.

By contributing you agree that your work is released under the project's
[MIT license](LICENSE).

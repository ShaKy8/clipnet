# Security policy

A clipboard manager sees everything you copy: passwords, tokens, private
messages. CLIP//NET is built to keep that on your machine and away from
anyone else. If you find a way around that, please report it privately.

## Reporting a vulnerability

Use GitHub's private
**[Report a vulnerability](https://github.com/ShaKy8/clipnet/security/advisories/new)**
form (the repository's **Security** tab). Only the maintainers can see the
report, and we can discuss it and agree on a fix before anything is public.
Please don't open a public issue or pull request for a vulnerability.

Helpful to include: the version (`daemon/clipnetd --version`), what an attacker
needs (another local user? a process running as you? a malicious clipboard
source?), the steps to reproduce, and what they get. Please don't send real
clipboard contents or your history database; demo data shows the problem just
as well.

This is a small project maintained in spare time, so replies are best-effort.
Fixes go into a new release, and the report is credited in the advisory unless
you'd rather it weren't.

## Supported versions

| Version | Supported |
|---|---|
| 1.0.x (the latest release) | ✅ |
| older | ❌ |

## What CLIP//NET protects against

- **Other users on the machine.** The history (`~/.local/share/clipnet`), its
  image and large-format files, exports and backups are private to you
  (directories 0700, files 0600). The daemon's socket lives in your private
  runtime directory, is itself 0600, and rejects any connection whose peer
  isn't your user id (`SO_PEERCRED`).
- **The network.** CLIP//NET never opens a network connection. It talks only
  to the Wayland compositor and Hyprland, over local Unix sockets. There is no
  sync, telemetry or update check.
- **Password managers.** Copies marked with `x-kde-passwordManagerHint` (set
  by KeePassXC and others) are never stored. Copies made while KeePassXC,
  1Password or Bitwarden has focus are excluded by default rules. After a
  password manager clears the clipboard on purpose, keep-alive never puts
  anything back.
- **Leftovers.** Deleted clips are overwritten inside the database
  (`secure_delete`), their files are removed once nothing refers to them, and
  the logs never contain clip contents.
- **User scripts.** Each Lua script runs in its own sandbox: no file,
  process, network or bytecode access, 64 MiB of memory, and 100 ms per call.
  Scripts start disabled.
- **Your config.** `clipnet install` writes only inside marked blocks in
  `~/.config/hypr`, backs files up first, and never touches `/usr/share`.
  `clipnet uninstall` removes all of it.

## Out of scope

These are real limits, but they are properties of the environment rather
than bugs in CLIP//NET:

- **Programs running as you.** Any process with your user id can read your
  files, talk to the daemon's socket, and (on Wayland, with data-control)
  read the clipboard directly. CLIP//NET can't defend against your own
  account.
- **The database is not encrypted at rest.** Use full-disk encryption, which
  Omarchy sets up by default.
- **Password copies the rules can't see.** The app exclusions match the
  *focused* window. A secret copied from a browser extension that doesn't set
  the password hint is attributed to the browser and stored. Use
  `clipnet pause`, a pause hotkey, or a rule for that case.
- **Your scripts' logs.** Anything your own script passes to `clipnet.log()`
  is written to the journal.

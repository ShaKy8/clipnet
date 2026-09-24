#!/usr/bin/env bash
# Managed config blocks: writing is idempotent, and removing restores the
# file byte for byte. Runs on copies of the real Hyprland files when present.
set -euo pipefail
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
# shellcheck source=../../scripts/clipnet
source "$ROOT/scripts/clipnet"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
check() { if "${@:2}"; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi; }

samples=()
for f in autostart bindings looknfeel; do
  src="${XDG_CONFIG_HOME:-$HOME/.config}/hypr/$f.lua"
  [[ -f $src ]] && cp "$src" "$T/$f.lua" && samples+=("$T/$f.lua")
done
printf 'first\n\nlast\n' > "$T/plain.lua"; samples+=("$T/plain.lua")
printf 'no newline at end' > "$T/nonl.lua"; samples+=("$T/nonl.lua")
printf '' > "$T/empty.lua"; samples+=("$T/empty.lua")

block='o.bind("CTRL + apostrophe", "Clipboard history", hl.dsp.global("clipnet:toggle"))
-- a line with a backslash \n and a $dollar'
for f in "${samples[@]}"; do
  name=$(basename "$f")
  # Once installed, the real files already hold our block: start the round
  # trip from the file as it would be without it.
  remove_block "$f"
  cp "$f" "$f.orig"
  write_block "$f" "$block"
  check "$name: block written" grep -qxF -e "$BEGIN_MARK" "$f"
  check "$name: content verbatim" grep -qF -- '-- a line with a backslash \n and a $dollar' "$f"
  cp "$f" "$f.once"
  write_block "$f" "$block"
  check "$name: rewriting is a no-op" cmp -s "$f" "$f.once"
  write_block "$f" "changed"
  check "$name: one block after a change" test "$(grep -cxF -e "$BEGIN_MARK" "$f")" = 1
  remove_block "$f"
  if [[ $name == nonl.lua ]]; then
    # awk ends every line with a newline; that is the one allowed difference.
    check "$name: removal restores the text" test "$(cat "$f")" = "$(cat "$f.orig")"
  else
    check "$name: removal restores the file exactly" cmp -s "$f" "$f.orig"
  fi
done
exit $fail

#!/usr/bin/env bash
#
# Live round trip against the running Wayland session, on a throwaway
# database: copy with wl-copy, check clipnetd stored every format, put the
# clip back with clipnetd, check wl-paste gets every format byte for byte.
#
# The clipboard is saved first and restored at the end (text only: an image
# on the clipboard when the test starts is not restored). A running
# clipnetd.service is left alone; the test daemon uses its own socket and
# data directory, but both daemons will see the test copies, so the real one
# stores them too unless it is paused.
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
BIN="${CLIPNETD:-$ROOT/daemon/clipnetd}"  # CLIPNETD: test another build (e.g. ASan)
[[ -x $BIN ]] || make -s -C "$ROOT/daemon"
[[ -n ${WAYLAND_DISPLAY:-} ]] || { echo "live_test: needs a Wayland session" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/clipnet-live.XXXXXX")
SOCK_DIR=$(mktemp -d "$XDG_RUNTIME_DIR/clipnet-live.XXXXXX")
SOCK="$SOCK_DIR/s.sock"
SAVED="$WORK/saved.txt"
had_text=false
if wl-paste -l 2>/dev/null | grep -qx 'text/plain;charset=utf-8\|UTF8_STRING\|text/plain'; then
  wl-paste -n > "$SAVED" 2>/dev/null && had_text=true
fi

# wl-copy forks into the background to serve the clipboard; keep it off our
# stdout/stderr or anything reading this script's output never sees EOF.
wlcopy() { command wl-copy "$@" >/dev/null 2>&1; }

pid=
cleanup() {
  local rc=$? status=0
  if [[ -n $pid ]]; then
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null || status=$?
  fi
  if $had_text; then wlcopy < "$SAVED"; fi
  # clipnetd exits 0 on SIGTERM; anything else (a crash, a sanitizer report)
  # fails the run even if every check passed.
  if ((status != 0)); then
    echo "clipnetd exited with status $status; its log:" >&2
    sed 's/^/  /' "$WORK/log" >&2
    rc=1
  fi
  rm -rf "$WORK" "$SOCK_DIR"
  exit "$rc"
}
trap cleanup EXIT

"$BIN" --data "$WORK/data" --socket "$SOCK" -v 2> "$WORK/log" &
pid=$!
for _ in $(seq 50); do [[ -S $SOCK ]] && break; sleep 0.05; done
ctl() { "$BIN" --socket "$SOCK" --ctl "$1"; }

fail=0
check() { # description, command...
  local what=$1; shift
  if "$@"; then echo "  ok   $what"; else echo "  FAIL $what"; fail=1; fi
}
count() { ctl '{"op":"state.get"}' | jq .count; }
wait_count() { # n, relative to the clips present when the daemon started
  for _ in $(seq 60); do
    [[ $(count) == $((base + $1)) ]] && return 0
    sleep 0.05
  done
  return 1
}

# The daemon stores whatever is on the clipboard when it starts; let that
# settle and count from there.
sleep 0.3
base=$(count)

echo "live: rich text"
html='<p>clip<b>net</b> live test — ✓ ünïcödé</p>'
printf '%s' "$html" | wlcopy -t text/html
check "html copy is captured" wait_count 1
row=$(ctl '{"op":"list","limit":1}' | jq '.rows[0]')
check "stored as rich text" test "$(jq -r .kind <<<"$row")" = rich
check "html format stored" jq -e '.mimes | index("text/html")' <<<"$row" >/dev/null
check "text format stored" jq -e '.mimes | index("text/plain;charset=utf-8")' <<<"$row" >/dev/null

echo "live: plain text, then serving the rich clip back"
printf 'plain second' | wlcopy
check "plain copy is captured" wait_count 2
id=$(jq .id <<<"$row")
ctl "{\"op\":\"copy\",\"ids\":[$id]}" >/dev/null
sleep 0.1
types=$(wl-paste -l)
check "offers text/html" grep -qx 'text/html' <<<"$types"
check "offers UTF8_STRING" grep -qx 'UTF8_STRING' <<<"$types"
check "offers the private marker" grep -qx 'application/x-clipnet-clip' <<<"$types"
check "html is byte-exact" test "$(wl-paste -n -t text/html)" = "$html"
sleep 0.2
check "serving does not create a new clip" wait_count 2

echo "live: plain-text paste mode"
ctl "{\"op\":\"copy\",\"ids\":[$id],\"mode\":\"plain\"}" >/dev/null
sleep 0.1
check "plain mode drops html" bash -c '! wl-paste -l | grep -qx text/html'

echo "live: Special Paste transforms the text it serves"
ctl "{\"op\":\"copy\",\"ids\":[$id],\"transform\":\"upper\"}" >/dev/null
sleep 0.1
check "transformed text is served" test "$(wl-paste -n)" = "CLIPNET LIVE TEST — ✓ ÜNÏCÖDÉ"
check "transformed paste offers no html" bash -c '! wl-paste -l | grep -qx text/html'
check "a transform is not stored as a new clip" wait_count 2

echo "live: image"
img="$ROOT/tests/integration/fixture.png"
wlcopy -t image/png < "$img"
check "image copy is captured" wait_count 3
irow=$(ctl '{"op":"list","limit":1}' | jq '.rows[0]')
check "stored as image" test "$(jq -r .kind <<<"$irow")" = image
check "thumbnail file exists" test -r "$(jq -r .image <<<"$irow")"
printf 'something else' | wlcopy
wait_count 4 || true
ctl "{\"op\":\"copy\",\"ids\":[$(jq .id <<<"$irow")]}" >/dev/null
sleep 0.1
check "image is byte-exact" cmp -s <(wl-paste -t image/png) "$img"

echo "live: password-manager hint and pause"
OFFER="$ROOT/daemon/build/tests/offer"
before=$(($(count) - base))
printf 'hunter2' | "$OFFER" --timeout 1500 'text/plain;charset=utf-8' x-kde-passwordManagerHint &
opid=$!
sleep 0.4
check "a copy carrying the password hint is not stored" wait_count "$before"
wait "$opid" || true
ctl '{"op":"pause"}' >/dev/null
printf 'secret while paused' | wlcopy
sleep 0.3
check "nothing captured while paused" wait_count "$before"
ctl '{"op":"resume"}' >/dev/null

echo "live: custom formats are kept and offered back"
printf 'custom-bytes' | "$OFFER" --timeout 1500 'text/plain;charset=utf-8' application/x-clipnet-test &
opid=$!
check "custom-format copy is captured" wait_count $((before + 1))
crow=$(ctl '{"op":"list","limit":1}' | jq '.rows[0]')
check "custom format stored" jq -e '.mimes | index("application/x-clipnet-test")' <<<"$crow" >/dev/null
ctl "{\"op\":\"copy\",\"ids\":[$(jq .id <<<"$crow")]}" >/dev/null
sleep 0.1
check "custom format offered back" test "$(wl-paste -n -t application/x-clipnet-test)" = custom-bytes
wait "$opid" || true
before=$((before + 1))

echo "live: rules drop formats and route clips"
g=$(ctl '{"op":"groups.create","name":"Routed"}' | jq .id)
guuid=$(ctl '{"op":"groups.list"}' | jq -r ".[] | select(.id==$g) | .uuid")
r1=$(ctl '{"op":"rules.set","action":"skip_mime","match_mime":"application/x-clipnet-test"}' | jq .id)
r2=$(ctl "{\"op\":\"rules.set\",\"action\":\"to_group\",\"match_app\":\"*\",\"arg\":{\"group\":\"$guuid\"}}" | jq .id)
printf 'ruled-bytes' | "$OFFER" --timeout 1500 'text/plain;charset=utf-8' application/x-clipnet-test &
opid=$!
check "ruled copy is captured" wait_count $((before + 1))
wait "$opid" || true
before=$((before + 1))
rrow=$(ctl '{"op":"list","limit":1}' | jq '.rows[0]')
if [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
  check "skip_mime dropped the format" bash -c "! jq -e '.mimes | index(\"application/x-clipnet-test\")' <<<'$rrow' >/dev/null"
  check "to_group routed the clip" test "$(jq .group_id <<<"$rrow")" = "$g"
fi
ctl "{\"op\":\"rules.delete\",\"id\":$r1}" >/dev/null
ctl "{\"op\":\"rules.delete\",\"id\":$r2}" >/dev/null

echo "live: PRIMARY (select-to-copy) is ignored unless enabled"
printf 'selected text one' | "$OFFER" --timeout 1500 --primary 'text/plain;charset=utf-8' &
opid=$!
sleep 1
check "PRIMARY ignored by default" wait_count "$before"
wait "$opid" || true
ctl '{"op":"settings.set","key":"capture_primary","value":true}' >/dev/null
printf 'selected text two' | "$OFFER" --timeout 2500 --primary 'text/plain;charset=utf-8' &
opid=$!
sleep 1.2
check "PRIMARY captured when enabled" wait_count $((before + 1))
check "PRIMARY clip holds the text" test "$(ctl '{"op":"list","limit":1}' | jq -r '.rows[0].preview')" = "selected text two"
wait "$opid" || true
ctl '{"op":"settings.set","key":"capture_primary","value":false}' >/dev/null
before=$((before + 1))

echo "live: keep-alive never touches a clipboard someone else owns"
printf 'still owned' | wlcopy
wait_count $((before + 1)) || true
before=$((before + 1))
restores=$(grep -c 'restoring clip' "$WORK/log" || true)
ctl '{"op":"show_context"}' >/dev/null
sleep 0.4
check "an owned clipboard is left alone" test "$(wl-paste -n 2>/dev/null)" = "still owned"
check "no restore was attempted" test "$(grep -c 'restoring clip' "$WORK/log" || true)" = "$restores"

echo "live: keep-alive after the owner exits"
printf 'owner goes away' | wl-copy --foreground >/dev/null 2>&1 &
wlpid=$!
wait_count $((before + 1)) || true
sleep 0.1
kill "$wlpid"; wait "$wlpid" 2>/dev/null || true
# A windowless owner exiting sends no event at all; opening the popup
# (show_context) is what checks, as it is right before a paste.
ctl '{"op":"show_context"}' >/dev/null
sleep 0.4
check "clip is still on the clipboard" test "$(wl-paste -n 2>/dev/null)" = "owner goes away"

if ((fail)); then
  echo "live test FAILED; daemon log:"; sed 's/^/  /' "$WORK/log"
  exit 1
fi
echo "live test passed"

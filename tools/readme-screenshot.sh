#!/usr/bin/env bash
#
# Regenerate docs/screenshot.png: the popup over invented demo clips, never
# the real history. A throwaway daemon (--no-wayland: it cannot read the
# clipboard) serves the demo data; the UI is pointed at it for a few seconds
# and then restarted on the real daemon, which keeps running throughout.
#
#   tools/readme-screenshot.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
BIN="$ROOT/daemon/clipnetd"
OUT="$ROOT/docs/screenshot.png"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/clipnet-demo.XXXXXX")
SOCK="$XDG_RUNTIME_DIR/clipnet-demo.sock"
make -s -C "$ROOT/daemon" clipnetd

ui_pattern="quickshell.* -p $ROOT\$"
demo_pid=
restore() {
  pkill -f "$ui_pattern" 2>/dev/null || true
  sleep 0.3
  [[ -n $demo_pid ]] && kill "$demo_pid" 2>/dev/null
  "$ROOT/scripts/clipnet" start >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap restore EXIT

# ---- demo data (all invented) ---------------------------------------------

magick -size 480x300 gradient:'#7d82d9'-'#060B1E' -swirl 120 \
  \( -size 480x300 plasma:fractal -blur 0x3 -colorspace Gray -level 30%,70% \) \
  -compose softlight -composite "$WORK/sky.png"
magick -size 320x200 xc:'#0b1026' -fill '#ffcead' -draw 'circle 160,100 160,40' \
  -fill '#7d82d9' -draw 'rectangle 0,150 320,200' "$WORK/moon.png"

python3 - "$WORK" <<'PY'
import base64, json, sys, time, uuid
work = sys.argv[1]
now = int(time.time() * 1000)
def b64(b): return base64.b64encode(b).decode()
def text(t): return {"mime": "text/plain;charset=utf-8", "aliases": ["text/plain;charset=utf-8", "UTF8_STRING", "text/plain"], "data": b64(t.encode())}
def html(h): return {"mime": "text/html", "data": b64(h.encode())}
def png(p): return {"mime": "image/png", "data": b64(open(f"{work}/{p}", "rb").read())}
g_snip, g_work = str(uuid.uuid4()), str(uuid.uuid4())
clips = []
def clip(minutes_ago, formats, app, **extra):
    t = now - minutes_ago * 60000
    clips.append(dict({"uuid": str(uuid.uuid4()), "created_at": t, "last_used_at": t, "source_app": app, "formats": formats}, **extra))
clip(2, [text("git clone https://github.com/ShaKy8/clipnet.git && cd clipnet && scripts/clipnet install")], "com.mitchellh.ghostty")
clip(6, [text("Meet at the trailhead at 7:30 — bring the thermos and the good map."), html("<p>Meet at the trailhead at <b>7:30</b> — bring the thermos and the good map.</p>")], "chromium")
clip(11, [png("sky.png")], "org.gnome.Loupe")
clip(19, [text("SELECT name, city FROM customers WHERE joined > '2026-01-01' ORDER BY name;")], "com.mitchellh.ghostty")
clip(34, [text("https://www.lua.org/manual/5.4/manual.html#6.4.1")], "chromium")
clip(52, [text("hyprctl clients | grep -i class")], "com.mitchellh.ghostty")
clip(75, [png("moon.png")], "org.gnome.Loupe")
clip(120, [text("Thanks so much for the recipe! The lemon bars were gone in ten minutes.")], "thunderbird")
clip(190, [text("sudo pacman -Syu && omarchy update")], "com.mitchellh.ghostty")
clip(300, [text("rsync -avh --progress ~/Photos/ /mnt/backup/photos/")], "com.mitchellh.ghostty")
clip(600, [text("Ditto was my favourite clipboard manager on Windows. This is it, for Linux.")], "org.gnome.TextEditor")
clip(1440, [text("function on_copy(clip)\n  if clip.text:match('^%d%d%d%d%d%d$') then return false end\nend")], "com.mitchellh.ghostty")
clip(2880, [text("Kind regards,\nA. Person\nWriter of Short Emails")], "thunderbird", title="Signature", quick_paste="sig", sticky=True, group=g_snip)
clip(4000, [text("1 Example Street, Springfield")], "chromium", title="Address (example)", group=g_work)
doc = {"format": "clipnet-export", "version": 1, "exported_at": now,
       "groups": [{"uuid": g_snip, "parent": None, "name": "Snippets"}, {"uuid": g_work, "parent": None, "name": "Work"}],
       "clips": clips}
json.dump(doc, open(f"{work}/demo.json", "w"))
PY

XDG_CONFIG_HOME="$WORK/config" "$BIN" --no-wayland --data "$WORK/data" --socket "$SOCK" 2>/dev/null &
demo_pid=$!
for _ in $(seq 50); do [[ -S $SOCK ]] && break; sleep 0.05; done
ctl() { "$BIN" --socket "$SOCK" --ctl "$1" >/dev/null; }
ctl "{\"op\":\"import\",\"format\":\"clipnet-json\",\"path\":\"$WORK/demo.json\"}"
ctl '{"op":"settings.set","key":"popup_position","value":"center"}'
# A fully opaque scrim turns everything around the card into one colour,
# so the card can be cut out exactly by trimming (and no desktop shows).
ctl '{"op":"settings.set","key":"scrim_alpha","value":100}'

# ---- the shot -----------------------------------------------------------------

pkill -f "$ui_pattern" 2>/dev/null || true
sleep 0.4
CLIPNET_SOCKET="$SOCK" setsid -f quickshell -n -d -p "$ROOT" >/dev/null 2>&1 </dev/null
sleep 1.5
hyprctl eval 'hl.dispatch(hl.dsp.global("clipnet:toggle"))' >/dev/null
sleep 0.6
layers() { hyprctl layers -j | jq '[.[].levels[][] | select(.namespace=="clipnet")] | length'; }
[[ $(layers) == 1 ]] || { echo "the popup did not open" >&2; exit 1; }
wtype -k Down          # past the sticky clip...
sleep 0.1
wtype -k Down          # ...to the rich-text one
sleep 0.1
wtype -k F3            # full view beside the list
sleep 0.7

grim -o "$(hyprctl monitors -j | jq -r '.[] | select(.focused) | .name')" "$WORK/full.png"
wtype -k Escape
sleep 0.1
[[ $(layers) == 1 ]] && wtype -k Escape

# Everything but the card is the scrim's single colour: trim to the card's
# border, then give it an even margin in that colour.
bg=$(magick "$WORK/full.png" -format '%[pixel:p{2,2}]' info:)
magick "$WORK/full.png" -fuzz 2% -trim +repage -bordercolor "$bg" -border 28 "$OUT"
echo "wrote $OUT ($(magick identify -format '%wx%h' "$OUT"))"

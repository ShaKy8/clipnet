pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io
import "ThemeParse.js" as Parse

// The current Omarchy theme, live.
//
// `omarchy-theme-set` replaces the whole theme directory and then rewrites
// `theme.name`, so the name file is what is watched: a watch on a file inside
// the replaced directory would go stale after the first switch.
Singleton {
  id: root

  readonly property string stateDir: Quickshell.env("HOME") + "/.local/state/omarchy/current"
  readonly property string userShell: Quickshell.env("HOME") + "/.config/omarchy/shell.toml"

  property var t: Parse.compute("", "", "")

  readonly property color foreground: t.foreground
  readonly property color background: t.background
  readonly property color accent: t.accent
  readonly property color urgent: t.urgent
  readonly property color muted: t.muted
  readonly property color card: t.card
  readonly property color text: t.text
  readonly property color border: t.border
  readonly property color scrim: t.scrim
  readonly property color selectedBackground: t.selectedBackground
  readonly property color selectedText: t.selectedText
  readonly property color selectedBorder: t.selectedBorder
  readonly property color dim: Qt.rgba(text.r, text.g, text.b, 0.55)
  readonly property color faint: Qt.rgba(text.r, text.g, text.b, 0.12)

  // Omarchy's fontconfig alias; `omarchy-font-set` points it at the chosen font.
  readonly property string fontFamily: "monospace"
  readonly property int fontSize: t.fontSize
  readonly property real scale: t.fontScale
  function px(n) { return Math.round(n * scale) }

  // Follows Hyprland's decoration:rounding (reported by the daemon on every
  // open), as Omarchy's own surfaces do.
  property int radius: 0
  readonly property int borderWidth: 2

  function recompute() {
    t = Parse.compute(colors.ok ? colors.text() : "", shell.ok ? shell.text() : "",
                      user.ok ? user.text() : "")
  }

  FileView {
    id: colors
    property bool ok: false
    path: root.stateDir + "/theme/colors.toml"
    printErrors: false
    onLoaded: { ok = true; root.recompute() }
    onLoadFailed: { ok = false; root.recompute() }
  }
  FileView {
    id: shell
    property bool ok: false
    path: root.stateDir + "/theme/shell.toml"
    printErrors: false
    onLoaded: { ok = true; root.recompute() }
    onLoadFailed: { ok = false; root.recompute() }
  }
  FileView {
    id: user
    property bool ok: false
    path: root.userShell
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: { ok = true; root.recompute() }
    onLoadFailed: { ok = false; root.recompute() }
  }
  FileView {
    path: root.stateDir + "/theme.name"
    watchChanges: true
    printErrors: false
    // The new directory is fully in place by the time the name is written.
    onFileChanged: { colors.reload(); shell.reload() }
  }

  function reload() { colors.reload(); shell.reload(); user.reload() }
}

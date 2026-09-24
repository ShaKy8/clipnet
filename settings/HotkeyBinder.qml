import QtQuick
import qs.common
import "../components"

// Binds one daemon hotkey action (e.g. buffer_copy 2) to keys, showing the
// current binding and any refusal (a key Hyprland already uses).
Column {
  id: hb

  property string label
  property string action
  property string arg: ""
  property var hotkeys: []       // the hotkeys.list result, shared by the page
  signal changed()

  readonly property var current: hotkeys.find(h => h.action === action && (h.arg || "") === arg) || null
  property string error: ""

  width: parent ? parent.width : 0
  spacing: Theme.px(2)

  // The field edits its own value when keys are captured; put it back to
  // what the daemon holds whenever that changes or a change is refused.
  function sync() { field.accel = current ? current.accel : "" }
  onCurrentChanged: sync()
  Component.onCompleted: sync()

  HotkeyField {
    id: field
    width: parent.width
    label: hb.label
    onChanged: a => {
      hb.error = ""
      const done = (r, err) => { hb.error = err ? err.message : ""; if (err) hb.sync(); hb.changed() }
      if (!a) {
        if (hb.current) Daemon.call("hotkeys.remove", { id: hb.current.id }, done)
        return
      }
      Daemon.call("hotkeys.set", { id: hb.current ? hb.current.id : 0, accel: a, action: hb.action, arg: hb.arg }, done)
    }
  }
  Text {
    visible: !!hb.error || !!(hb.current && hb.current.conflict)
    width: parent.width
    wrapMode: Text.Wrap
    text: hb.error || (hb.current && hb.current.conflict ? "Not active: taken by “" + hb.current.conflict + "”" : "")
    color: Theme.urgent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
}

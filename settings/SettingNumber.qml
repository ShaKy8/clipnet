import QtQuick
import qs.common
import "../components/ui"

// A whole-number daemon setting, edited in a unit (e.g. MB for bytes).
// Saved on Enter or when the field loses focus.
Column {
  id: n

  property string key
  property string label
  property string hint: ""
  property string unit: ""
  property real scale: 1        // stored value = shown value × scale
  property string error: ""

  width: parent ? parent.width : 0
  spacing: Theme.px(4)

  function shown() { return String(Math.round(Daemon.setting(key, 0) / scale)) }
  function commit() {
    if (field.text === shown()) return
    const v = parseInt(field.text, 10)
    if (isNaN(v)) { field.text = shown(); return }
    Daemon.call("settings.set", { key: key, value: v * scale }, (r, err) => {
      n.error = err ? err.message : ""
      if (err) field.text = shown()
    })
  }

  Connections { target: Daemon; function onSettingsChanged() { if (!field.input.activeFocus) field.text = n.shown() } }
  Component.onCompleted: field.text = shown()

  Row {
    spacing: Theme.px(8)
    Field {
      id: field
      width: Theme.px(120)
      label: n.label
      numeric: true
      onAccepted: n.commit()
      input.onActiveFocusChanged: if (!input.activeFocus) n.commit()
    }
    Text {
      anchors.bottom: parent.bottom
      anchors.bottomMargin: Theme.px(8)
      text: n.unit
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 1
    }
  }
  Text {
    visible: !!n.hint || !!n.error
    width: parent.width
    wrapMode: Text.Wrap
    text: n.error || n.hint
    color: n.error ? Theme.urgent : Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
}

import QtQuick
import qs.common
import "../common/Keys.js" as KeyMap

// Click (or focus and press Enter), then press the combination. Backspace or
// Delete clears it, Esc stops listening without changing it.
Column {
  id: h

  property string label: "Hotkey"
  property string accel: ""   // portable form, "" for none
  property bool listening: false
  property string held: ""
  signal changed(string accel)

  spacing: Theme.px(4)

  Text {
    text: h.label
    color: Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
  Rectangle {
    id: box
    width: h.width
    height: Math.round(Theme.fontSize * 2.2)
    radius: Math.min(Theme.radius, 6)
    color: h.listening ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15) : Theme.faint
    border.width: box.activeFocus ? 2 : 1
    border.color: box.activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.2)
    activeFocusOnTab: true
    onActiveFocusChanged: if (!activeFocus) h.listening = false

    Text {
      anchors.left: parent.left
      anchors.leftMargin: Theme.px(8)
      anchors.verticalCenter: parent.verticalCenter
      text: h.listening ? (h.held ? KeyMap.pretty(h.held) + "…" : "Press the keys…  (Esc to stop)")
          : h.accel ? KeyMap.pretty(h.accel) : "None — click to set"
      color: h.accel || h.listening ? Theme.text : Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
    }
    Text {
      visible: !!h.accel && !h.listening
      anchors.right: parent.right
      anchors.rightMargin: Theme.px(8)
      anchors.verticalCenter: parent.verticalCenter
      text: ""
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      MouseArea { anchors.fill: parent; anchors.margins: -4; onClicked: { h.accel = ""; h.changed("") } }
    }
    MouseArea {
      anchors.fill: parent
      anchors.rightMargin: Theme.px(28)
      onClicked: { box.forceActiveFocus(); h.listening = true; h.held = "" }
    }

    Keys.priority: Keys.BeforeItem
    Keys.onPressed: e => {
      if (!h.listening) {
        if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter || e.key === Qt.Key_Space) { h.listening = true; h.held = ""; e.accepted = true }
        else if (e.key === Qt.Key_Backspace || e.key === Qt.Key_Delete) { h.accel = ""; h.changed(""); e.accepted = true }
        return
      }
      e.accepted = true
      if (e.key === Qt.Key_Escape && !e.modifiers) { h.listening = false; return }
      if ((e.key === Qt.Key_Backspace || e.key === Qt.Key_Delete) && !e.modifiers) {
        h.accel = ""; h.listening = false; h.changed(""); return
      }
      if (KeyMap.isModifier(Qt, e.key)) {
        const m = e.modifiers
        h.held = (m & Qt.MetaModifier ? "Super+" : "") + (m & Qt.ControlModifier ? "Ctrl+" : "") +
                 (m & Qt.AltModifier ? "Alt+" : "") + (m & Qt.ShiftModifier ? "Shift+" : "")
        return
      }
      const a = KeyMap.accel(Qt, e)
      if (!a) return
      h.accel = a
      h.listening = false
      h.changed(a)
    }
  }
}

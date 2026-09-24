import QtQuick
import qs.common

// A daemon setting with a few named values, shown as segmented buttons.
Column {
  id: c

  property string key
  property string label
  property var options: []   // [{ value, label }]

  width: parent ? parent.width : 0
  spacing: Theme.px(4)

  Text {
    text: c.label
    color: Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
  Row {
    spacing: Theme.px(4)
    Repeater {
      model: c.options
      Rectangle {
        required property var modelData
        readonly property bool on: Daemon.setting(c.key, "") === modelData.value
        activeFocusOnTab: true
        width: lbl.implicitWidth + Theme.px(20)
        height: Math.round(Theme.fontSize * 2.1)
        radius: Math.min(Theme.radius, 6)
        color: on ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.3) : Theme.faint
        border.width: activeFocus ? 2 : 1
        border.color: activeFocus || on ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.2)
        Text {
          id: lbl
          anchors.centerIn: parent
          text: modelData.label
          color: Theme.text
          font.family: Theme.fontFamily
          font.pixelSize: Theme.fontSize
        }
        function pick() { Daemon.call("settings.set", { key: c.key, value: modelData.value }) }
        MouseArea { anchors.fill: parent; onClicked: parent.pick() }
        Keys.onPressed: e => { if (e.key === Qt.Key_Space || e.key === Qt.Key_Return) { pick(); e.accepted = true } }
      }
    }
  }
}

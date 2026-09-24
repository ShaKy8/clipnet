import QtQuick
import qs.common

// A themed push button. Space/Enter activate it when focused.
Rectangle {
  id: btn

  property string text: ""
  property bool primary: false
  property bool danger: false
  signal clicked()

  activeFocusOnTab: true
  implicitWidth: Math.max(Theme.px(72), label.implicitWidth + Theme.px(24))
  implicitHeight: Math.round(Theme.fontSize * 2.2)
  radius: Math.min(Theme.radius, 6)
  color: primary ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, mouse.pressed ? 0.45 : 0.3)
       : mouse.containsMouse || activeFocus ? Theme.faint : "transparent"
  border.width: activeFocus ? 2 : 1
  border.color: danger ? Theme.urgent : activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.3)

  Text {
    id: label
    anchors.centerIn: parent
    text: btn.text
    color: btn.danger ? Theme.urgent : Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
  }
  MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: btn.clicked() }
  Keys.onPressed: e => {
    if (e.key === Qt.Key_Space || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { btn.clicked(); e.accepted = true }
  }
}

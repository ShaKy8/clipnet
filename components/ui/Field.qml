import QtQuick
import qs.common

// A themed single-line text field with an optional label above it.
Column {
  id: f

  property string label: ""
  property string placeholder: ""
  property alias text: input.text
  property alias input: input
  property bool numeric: false
  signal accepted()

  spacing: Theme.px(4)

  function focusField() { input.forceActiveFocus(); input.selectAll() }

  Text {
    visible: !!f.label
    text: f.label
    color: Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
  Rectangle {
    width: f.width
    height: Math.round(Theme.fontSize * 2.2)
    radius: Math.min(Theme.radius, 6)
    color: Theme.faint
    border.width: input.activeFocus ? 2 : 1
    border.color: input.activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.2)
    TextInput {
      id: input
      anchors.fill: parent
      anchors.leftMargin: Theme.px(8)
      anchors.rightMargin: Theme.px(8)
      verticalAlignment: TextInput.AlignVCenter
      clip: true
      activeFocusOnTab: true
      color: Theme.text
      selectionColor: Theme.accent
      selectedTextColor: Theme.card
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      inputMethodHints: f.numeric ? Qt.ImhDigitsOnly : Qt.ImhNone
      validator: f.numeric ? intValidator : null
      onAccepted: f.accepted()
      Text {
        visible: !input.text && !!f.placeholder
        anchors.verticalCenter: parent.verticalCenter
        text: f.placeholder
        color: Theme.dim
        font: input.font
      }
    }
    IntValidator { id: intValidator; bottom: 0 }
  }
}

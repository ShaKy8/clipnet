import QtQuick
import qs.common
import "ui"

// Ask for one line of text (a group name). Enter accepts, Esc cancels.
Sheet {
  id: p

  property string label: ""
  property string initial: ""
  property string okText: "OK"
  property string error: ""
  signal accepted(string text)
  signal cancelled()

  onCloseRequested: cancelled()
  Component.onCompleted: { field.text = initial; field.focusField() }

  Field {
    id: field
    width: parent.width
    label: p.label
    onAccepted: p.accepted(text)
    input.Keys.onEscapePressed: p.cancelled()
  }
  Text {
    visible: !!p.error
    width: parent.width
    wrapMode: Text.Wrap
    text: p.error
    color: Theme.urgent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 1
  }
  Row {
    anchors.right: parent.right
    spacing: Theme.px(8)
    Btn { text: "Cancel"; onClicked: p.cancelled() }
    Btn { text: p.okText; primary: true; onClicked: p.accepted(field.text) }
  }
}

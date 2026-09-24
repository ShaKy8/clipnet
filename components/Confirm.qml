import QtQuick
import qs.common
import "ui"

// Yes/no. Enter confirms, Esc cancels.
Sheet {
  id: c

  property string message: ""
  property string okText: "OK"
  property bool danger: false
  property string altText: ""   // optional third choice (e.g. "Delete clips too")
  signal confirmed()
  signal alternative()
  signal cancelled()

  onCloseRequested: cancelled()
  Component.onCompleted: keys.forceActiveFocus()

  Text {
    width: parent.width
    wrapMode: Text.Wrap
    text: c.message
    color: Theme.text
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize
  }
  Row {
    anchors.right: parent.right
    spacing: Theme.px(8)
    Btn { text: "Cancel"; onClicked: c.cancelled() }
    Btn { visible: !!c.altText; text: c.altText; danger: true; onClicked: c.alternative() }
    Btn { text: c.okText; primary: true; danger: c.danger; onClicked: c.confirmed() }
  }
  Item {
    id: keys
    focus: true
    Keys.onPressed: e => {
      if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) c.confirmed()
      else if (e.key === Qt.Key_Escape) c.cancelled()
      else return
      e.accepted = true
    }
  }
}

import QtQuick
import qs.common

// A titled block of settings.
Column {
  id: s
  property string title
  default property alias content: body.data
  width: parent ? parent.width : 0
  spacing: Theme.px(10)
  Text {
    text: s.title
    color: Theme.accent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize + 1
    font.bold: true
  }
  Column {
    id: body
    width: parent.width
    spacing: Theme.px(12)
  }
}

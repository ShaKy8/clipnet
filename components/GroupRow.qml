import QtQuick
import qs.common

// A group (folder) row, listed above the clips of the group being viewed.
Item {
  id: row

  required property int index
  required property var modelData
  property bool current: false
  property int lineHeight: Theme.px(24)

  signal clicked()
  signal doubleClicked()
  signal rightClicked()

  height: lineHeight

  Rectangle {
    anchors.fill: parent
    color: row.current ? Theme.selectedBackground : hover.hovered ? Theme.faint : "transparent"
    border.width: row.current && Theme.selectedBorder.a > 0 ? 1 : 0
    border.color: Theme.selectedBorder
    radius: Math.min(Theme.radius, 6)
  }
  HoverHandler { id: hover }
  MouseArea {
    anchors.fill: parent
    acceptedButtons: Qt.LeftButton | Qt.RightButton
    onClicked: mouse => mouse.button === Qt.RightButton ? row.rightClicked() : row.clicked()
    onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) row.doubleClicked() }
  }
  Row {
    anchors.fill: parent
    anchors.leftMargin: Theme.px(6)
    anchors.rightMargin: Theme.px(8)
    spacing: Theme.px(8)
    Item { width: Theme.px(12); height: 1 }
    Text {
      width: Theme.px(14)
      height: row.height
      verticalAlignment: Text.AlignVCenter
      horizontalAlignment: Text.AlignHCenter
      text: ""
      color: Theme.accent
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
    }
    Text {
      width: parent.width - x - count.width - parent.spacing
      height: row.height
      verticalAlignment: Text.AlignVCenter
      elide: Text.ElideRight
      text: row.modelData.name
      color: row.current ? Theme.selectedText : Theme.text
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      font.bold: true
    }
    Text {
      id: count
      height: row.height
      verticalAlignment: Text.AlignVCenter
      text: row.modelData.count + (row.modelData.count === 1 ? " clip" : " clips")
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 2
    }
  }
}

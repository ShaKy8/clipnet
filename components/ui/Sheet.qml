import QtQuick
import qs.common

// A dialog drawn inside the popup card: dims the card, centres a panel, and
// keeps keyboard focus inside itself until it closes.
Item {
  id: sheet

  property string title: ""
  property int panelWidth: Theme.px(420)
  default property alias content: body.data
  property alias panel: panel
  signal closeRequested()

  anchors.fill: parent
  z: 50

  Rectangle {
    anchors.fill: parent
    radius: Theme.radius
    color: Qt.rgba(Theme.card.r, Theme.card.g, Theme.card.b, 0.75)
    MouseArea { anchors.fill: parent; onClicked: sheet.closeRequested() }
  }

  Rectangle {
    id: panel
    anchors.centerIn: parent
    width: Math.min(sheet.panelWidth, sheet.width - Theme.px(24))
    height: Math.min(col.implicitHeight + Theme.px(28), sheet.height - Theme.px(24))
    color: Theme.card
    radius: Theme.radius
    border.width: Theme.borderWidth
    border.color: Theme.accent
    clip: true
    MouseArea { anchors.fill: parent }

    Column {
      id: col
      x: Theme.px(14)
      y: Theme.px(14)
      width: parent.width - Theme.px(28)
      spacing: Theme.px(10)
      Text {
        visible: !!sheet.title
        text: sheet.title
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize + 2
        font.bold: true
      }
      Column {
        id: body
        width: parent.width
        spacing: Theme.px(10)
      }
    }
  }
}

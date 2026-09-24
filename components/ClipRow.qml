import QtQuick
import qs.common

// One row of the list: quick-paste number, kind glyph (or a thumbnail),
// the one-line preview, badges, and how long ago it was used.
Item {
  id: row

  required property int index
  required property var modelData
  // Position among the clips in the list (group rows come first and are not
  // numbered), for Ditto's Ctrl+1..0 labels.
  property int position: index
  property bool current: false
  property bool selected: false
  property bool showThumbnails: true
  property int lineHeight: Theme.px(24)
  property real now: Date.now()

  signal hoverChanged(bool hovering)
  signal clicked(var mouse)
  signal doubleClicked()
  signal rightClicked()

  readonly property bool isImage: modelData.kind === "image" && !!modelData.image && showThumbnails
  height: isImage ? lineHeight * 3 : lineHeight

  Rectangle {
    anchors.fill: parent
    color: row.current ? Theme.selectedBackground
         : row.selected ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.14)
         : hover.hovered ? Theme.faint : "transparent"
    border.width: row.current && Theme.selectedBorder.a > 0 ? 1 : 0
    border.color: Theme.selectedBorder
    radius: Math.min(Theme.radius, 6)
  }

  HoverHandler { id: hover; onHoveredChanged: row.hoverChanged(hovered) }

  MouseArea {
    anchors.fill: parent
    acceptedButtons: Qt.LeftButton | Qt.RightButton
    onClicked: mouse => mouse.button === Qt.RightButton ? row.rightClicked() : row.clicked(mouse)
    onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) row.doubleClicked() }
  }

  Row {
    anchors.fill: parent
    anchors.leftMargin: Theme.px(6)
    anchors.rightMargin: Theme.px(8)
    spacing: Theme.px(8)

    // Ctrl+1..9, Ctrl+0: Ditto numbers the first ten rows.
    Text {
      width: Theme.px(12)
      height: row.lineHeight
      verticalAlignment: Text.AlignVCenter
      horizontalAlignment: Text.AlignRight
      text: Format.positionLabel(row.position)
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 1
    }

    Item {
      width: row.isImage ? height * 1.6 : Theme.px(14)
      height: row.height
      Text {
        visible: !row.isImage
        anchors.centerIn: parent
        text: row.modelData.sticky ? "" : Format.glyph(row.modelData.kind, true)
        color: row.modelData.sticky ? Theme.accent : Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
      }
      Image {
        visible: row.isImage
        anchors.fill: parent
        anchors.margins: Theme.px(3)
        source: row.isImage ? "file://" + row.modelData.image : ""
        sourceSize.height: height * 2
        fillMode: Image.PreserveAspectFit
        horizontalAlignment: Image.AlignLeft
        asynchronous: true
        cache: true
        smooth: true
      }
    }

    Text {
      id: label
      width: parent.width - x - meta.width - parent.spacing
      height: row.height
      verticalAlignment: Text.AlignVCenter
      elide: Text.ElideRight
      textFormat: Text.PlainText
      maximumLineCount: 1
      text: row.modelData.title || row.modelData.preview
      color: row.current ? Theme.selectedText : Theme.text
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      font.italic: !!row.modelData.title
    }

    Row {
      id: meta
      height: row.height
      spacing: Theme.px(6)
      Text {
        visible: !!row.modelData.quick_paste
        anchors.verticalCenter: parent.verticalCenter
        text: "→" + (row.modelData.quick_paste || "")
        color: Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
      }
      Text {
        visible: !!row.modelData.hotkey
        anchors.verticalCenter: parent.verticalCenter
        text: row.modelData.hotkey || ""
        color: Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
      }
      Text {
        visible: row.modelData.locked
        anchors.verticalCenter: parent.verticalCenter
        text: ""
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
      }
      Text {
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.px(30)
        horizontalAlignment: Text.AlignRight
        text: Format.age(row.modelData.last_used_at, row.now)
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
      }
    }
  }
}

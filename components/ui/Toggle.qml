import QtQuick
import qs.common

// A labelled on/off switch. Space toggles it when focused.
Item {
  id: t

  property string text: ""
  property string hint: ""
  property bool checked: false
  signal toggled(bool checked)

  activeFocusOnTab: true
  implicitHeight: Math.max(Math.round(Theme.fontSize * 2), col.implicitHeight)
  implicitWidth: box.width + col.implicitWidth + Theme.px(10)

  function flip() { checked = !checked; toggled(checked) }

  Rectangle {
    id: box
    anchors.left: parent.left
    anchors.verticalCenter: col.lines > 1 ? undefined : parent.verticalCenter
    anchors.top: col.lines > 1 ? parent.top : undefined
    anchors.topMargin: Theme.px(2)
    width: Math.round(Theme.fontSize * 2.4)
    height: Math.round(Theme.fontSize * 1.3)
    radius: height / 2
    color: t.checked ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.55) : Theme.faint
    border.width: t.activeFocus ? 2 : 1
    border.color: t.activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.3)
    Rectangle {
      width: parent.height - 6
      height: width
      radius: width / 2
      y: 3
      x: t.checked ? parent.width - width - 3 : 3
      color: t.checked ? Theme.text : Theme.dim
    }
  }
  Column {
    id: col
    property int lines: t.hint ? 2 : 1
    anchors.left: box.right
    anchors.leftMargin: Theme.px(10)
    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    Text {
      width: parent.width
      text: t.text
      color: Theme.text
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      wrapMode: Text.Wrap
    }
    Text {
      visible: !!t.hint
      width: parent.width
      text: t.hint
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 2
      wrapMode: Text.Wrap
    }
  }
  MouseArea { anchors.fill: parent; onClicked: { t.forceActiveFocus(); t.flip() } }
  Keys.onPressed: e => { if (e.key === Qt.Key_Space) { t.flip(); e.accepted = true } }
}

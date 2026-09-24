import QtQuick
import qs.common
import "ui"

// Edit a clip's text, or write a new clip (Ditto's Ctrl+E / Ctrl+N).
// Ctrl+S or Ctrl+Enter saves; Esc cancels (asking first if anything changed).
Sheet {
  id: ed

  property var clip: null          // null: a new clip
  property string initialText: ""
  property string error: ""
  property bool confirmDiscard: false
  readonly property bool changed: area.text !== initialText
  readonly property bool loses: !!clip && (clip.mimes || []).some(m => m !== "text/plain;charset=utf-8")
  signal saved(string text)
  signal cancelled()

  title: clip ? "Edit clip" : "New clip"
  panelWidth: Theme.px(640)

  function cancel() {
    if (changed && !confirmDiscard) { confirmDiscard = true; error = "Unsaved changes. Press Esc again to discard them."; return }
    cancelled()
  }
  function save() {
    if (!area.text.length) { error = "The clip is empty."; return }
    saved(area.text)
  }

  onCloseRequested: cancel()
  Component.onCompleted: { area.text = initialText; area.forceActiveFocus(); area.cursorPosition = area.text.length }

  Text {
    visible: ed.loses
    width: parent.width
    wrapMode: Text.Wrap
    text: "Saving keeps only the text: this clip's other formats (" + (ed.clip ? ed.clip.mimes.filter(m => m !== "text/plain;charset=utf-8").join(", ") : "") + ") will be dropped."
    color: Theme.accent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
  Rectangle {
    width: parent.width
    height: Math.min(Theme.px(360), Math.max(Theme.px(160), ed.height * 0.6))
    color: Theme.faint
    radius: Math.min(Theme.radius, 6)
    border.width: area.activeFocus ? 2 : 1
    border.color: area.activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.2)
    Flickable {
      id: flick
      anchors.fill: parent
      anchors.margins: Theme.px(8)
      contentWidth: width
      contentHeight: area.implicitHeight
      clip: true
      boundsBehavior: Flickable.StopAtBounds
      function ensureVisible(r) {
        if (contentY >= r.y) contentY = r.y
        else if (contentY + height <= r.y + r.height) contentY = r.y + r.height - height
      }
      TextEdit {
        id: area
        width: flick.width
        wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
        textFormat: TextEdit.PlainText
        selectByMouse: true
        persistentSelection: true
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.card
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
        onTextChanged: { ed.confirmDiscard = false; if (ed.error) ed.error = "" }
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: e => {
          const ctrl = e.modifiers & Qt.ControlModifier
          if (ctrl && (e.key === Qt.Key_S || e.key === Qt.Key_Return || e.key === Qt.Key_Enter)) { ed.save(); e.accepted = true }
          else if (e.key === Qt.Key_Escape) { ed.cancel(); e.accepted = true }
        }
      }
    }
  }
  Text {
    visible: !!ed.error
    width: parent.width
    wrapMode: Text.Wrap
    text: ed.error
    color: Theme.urgent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 1
  }
  Item {
    width: parent.width
    height: buttons.height
    Text {
      anchors.left: parent.left
      anchors.verticalCenter: parent.verticalCenter
      text: area.text.length + " characters · Ctrl+S saves · Esc cancels"
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 2
    }
    Row {
      id: buttons
      anchors.right: parent.right
      spacing: Theme.px(8)
      Btn { text: "Cancel"; onClicked: ed.cancel() }
      Btn { text: "Save"; primary: true; onClicked: ed.save() }
    }
  }
}

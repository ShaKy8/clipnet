import QtQuick
import qs.common

// Ditto's "view full description" (F3), as a pane beside the list: the
// whole text (W toggles wrapping), the image at size, or the file list, and
// what the clip is made of underneath.
Rectangle {
  id: pane

  property var detail: null
  property bool wrap: true

  color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.035)
  radius: Math.min(Theme.radius, 6)
  clip: true

  readonly property bool isImage: !!detail && detail.kind === "image" && !!detail.image

  function scroll(lines) {
    flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + lines * Theme.fontSize * 1.4))
  }

  Flickable {
    id: flick
    anchors.fill: parent
    anchors.margins: Theme.px(10)
    anchors.bottomMargin: info.height + Theme.px(14)
    contentWidth: pane.isImage ? width : (pane.wrap ? width : Math.max(width, body.implicitWidth))
    contentHeight: pane.isImage ? height : body.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    TextEdit {
      id: body
      visible: !pane.isImage
      width: pane.wrap ? flick.width : implicitWidth
      readOnly: true
      selectByMouse: true
      textFormat: TextEdit.PlainText
      wrapMode: pane.wrap ? TextEdit.WrapAtWordBoundaryOrAnywhere : TextEdit.NoWrap
      color: Theme.text
      selectionColor: Theme.accent
      selectedTextColor: Theme.card
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize
      // Very long clips are cut for display only; paste uses the real bytes.
      text: !pane.detail ? "" : (pane.detail.text || "").length > 200000
            ? pane.detail.text.substring(0, 200000) + "\n\n[… display truncated]" : (pane.detail.text || pane.detail.preview || "")
    }

    Image {
      visible: pane.isImage
      width: flick.width
      height: flick.height
      source: pane.isImage ? "file://" + pane.detail.image : ""
      fillMode: Image.PreserveAspectFit
      asynchronous: true
      smooth: true
      mipmap: true
    }
  }

  Text {
    id: info
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: Theme.px(10)
    wrapMode: Text.Wrap
    color: Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
    textFormat: Text.PlainText
    text: {
      const d = pane.detail
      if (!d) return ""
      const parts = [d.kind, Format.size(d.size), Format.stamp(d.created_at)]
      if (d.source_app) parts.push(d.source_app)
      if (d.paste_count) parts.push("pasted " + d.paste_count + "×")
      const mimes = (d.formats || []).map(f => f.mime + " (" + Format.size(f.size) + ")")
      return parts.join(" · ") + "\n" + mimes.join(", ")
    }
  }
}

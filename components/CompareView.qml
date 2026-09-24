import QtQuick
import qs.common
import "ui"
import "../common/Diff.js" as Diff

// Two clips side by side, lines aligned, changes marked (Ditto's Ctrl+F2
// opens an external diff tool; this is built in). Esc closes, ↑↓ PgUp PgDn
// scroll, N / P jump to the next / previous change.
Sheet {
  id: cv

  property var leftClip: null    // clip details (from `get`)
  property var rightClip: null
  readonly property var diff: Diff.compare(leftClip ? leftClip.text : "", rightClip ? rightClip.text : "")
  readonly property var counts: Diff.summary(diff.rows)
  signal cancelled()
  readonly property int wantWidth: Theme.px(1000)   // the popup widens for it

  title: "Compare"
  panelWidth: Math.max(Theme.px(640), parent ? parent.width - Theme.px(24) : 0)
  onCloseRequested: cancelled()
  Component.onCompleted: keys.forceActiveFocus()

  function label(c) { return c ? (c.title || c.preview || "").substring(0, 60) : "" }
  function jump(dir) {
    const rows = diff.rows, rh = Math.round(Theme.fontSize * 1.5)
    let i = Math.floor(view.contentY / rh) + dir
    for (; i >= 0 && i < rows.length; i += dir) {
      if (rows[i].kind !== "same" && (i === 0 || rows[i - 1].kind === "same" || dir < 0)) {
        view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, (i - 2) * rh))
        return
      }
    }
  }

  Row {
    width: parent.width
    Text {
      width: parent.width / 2
      elide: Text.ElideRight
      text: "◀ " + cv.label(cv.leftClip)
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 1
    }
    Text {
      width: parent.width / 2
      elide: Text.ElideRight
      text: "▶ " + cv.label(cv.rightClip)
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 1
    }
  }

  Rectangle {
    width: parent.width
    height: Math.max(Theme.px(160), (cv.parent ? cv.parent.height : 400) - Theme.px(150))
    color: Theme.faint
    radius: Math.min(Theme.radius, 6)
    clip: true
    ListView {
      id: view
      anchors.fill: parent
      anchors.margins: Theme.px(4)
      model: cv.diff.rows
      boundsBehavior: Flickable.StopAtBounds
      delegate: Row {
        required property var modelData
        width: view.width
        height: Math.round(Theme.fontSize * 1.5)
        Repeater {
          model: [modelData.left, modelData.right]
          Rectangle {
            required property var modelData
            required property int index
            readonly property string kind: parent.modelData ? parent.modelData.kind : "same"
            width: view.width / 2
            height: parent.height
            color: kind === "same" ? "transparent"
                 : modelData === null ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.03)
                 : index === 0 ? Qt.rgba(Theme.urgent.r, Theme.urgent.g, Theme.urgent.b, 0.18)
                 : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.22)
            Text {
              anchors.fill: parent
              anchors.leftMargin: Theme.px(6)
              verticalAlignment: Text.AlignVCenter
              elide: Text.ElideRight
              textFormat: Text.PlainText
              text: modelData === null ? "" : modelData
              color: Theme.text
              font.family: Theme.fontFamily
              font.pixelSize: Theme.fontSize - 1
            }
          }
        }
      }
    }
  }
  Text {
    width: parent.width
    wrapMode: Text.Wrap
    text: cv.counts.same + " same · " + cv.counts.changed + " changed · " + cv.counts.removed + " only left · " +
          cv.counts.added + " only right" + (cv.diff.aligned ? "" : " · long clips: lines paired by position") +
          "   N/P next/previous change · Esc closes"
    color: Theme.dim
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 2
  }
  Item {
    id: keys
    focus: true
    Keys.onPressed: e => {
      const step = Math.round(Theme.fontSize * 1.5)
      if (e.key === Qt.Key_Escape) cv.cancelled()
      else if (e.key === Qt.Key_Down) view.contentY = Math.min(view.contentHeight - view.height, view.contentY + step)
      else if (e.key === Qt.Key_Up) view.contentY = Math.max(0, view.contentY - step)
      else if (e.key === Qt.Key_PageDown) view.contentY = Math.min(view.contentHeight - view.height, view.contentY + view.height)
      else if (e.key === Qt.Key_PageUp) view.contentY = Math.max(0, view.contentY - view.height)
      else if (e.key === Qt.Key_N) cv.jump(1)
      else if (e.key === Qt.Key_P) cv.jump(-1)
      else return
      e.accepted = true
    }
  }
}

import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.common

// The clipboard history popup: Ditto's list, in the Omarchy theme.
//
// A transparent full-screen overlay on the monitor under the cursor, with the
// card placed at the cursor (or centered, or where it was last). Clicking
// outside the card closes it. Keyboard focus is exclusive while open, and
// focus returns to the previous window as soon as the surface unmaps, which
// is what the daemon waits for before it sends the paste keystroke.
PanelWindow {
  id: popup

  property bool open: false
  property var rows: []
  property int total: 0
  property string query: ""
  property bool previewOpen: false
  property var detail: null
  property bool stale: true
  property string offlineText: ""
  // Rows picked with Ctrl/Shift for a multi-paste, by clip id.
  property var picked: ({})
  property int pickAnchor: -1
  property real cardX: 0
  property real cardY: 0
  property var lastPos: ({})

  readonly property int rowHeight: Math.round(Theme.fontSize * 2)
  readonly property int visibleRows: Daemon.setting("popup_rows", 14)
  readonly property int listWidth: Theme.px(Daemon.setting("popup_width", 520))
  readonly property int paneWidth: Theme.px(440)
  readonly property int pad: Theme.px(8)
  readonly property var currentRow: rows.length ? rows[Math.max(0, Math.min(list.currentIndex, rows.length - 1))] : null

  visible: open
  color: "transparent"
  anchors { top: true; bottom: true; left: true; right: true }
  exclusionMode: ExclusionMode.Ignore
  WlrLayershell.namespace: "clipnet"
  WlrLayershell.layer: WlrLayer.Overlay
  WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

  // ---- showing and hiding ------------------------------------------------

  function toggle() { open ? hide() : show() }

  function show() {
    Daemon.call("show_context", {}, (ctx, err) => {
      place(ctx || {})
      search.text = ""
      query = ""
      picked = {}
      pickAnchor = -1
      list.currentIndex = 0
      offlineText = err ? "clipnetd is not running — start it with: clipnet start" : ""
      if (stale || !rows.length) refresh(true)
      open = true
      search.forceActiveFocus()
      ticker.now = Date.now()
    })
  }

  function hide() {
    if (!open) return
    lastPos[screen ? screen.name : ""] = { x: cardX, y: cardY }
    open = false
  }

  // Put the card on the monitor under the cursor, at the cursor, clamped so
  // it never hangs off an edge.
  function place(ctx) {
    if (ctx.rounding !== undefined) Theme.radius = ctx.rounding
    let mon = null
    if (ctx.monitor && ctx.monitor.name) {
      for (const s of Quickshell.screens) if (s.name === ctx.monitor.name) mon = s
    }
    if (mon && mon !== screen) screen = mon
    const w = mon ? mon.width : width
    const h = mon ? mon.height : height
    const cw = card.width, ch = card.height
    const mode = Daemon.setting("popup_position", "cursor")
    let x = (w - cw) / 2, y = (h - ch) / 3
    if (mode === "cursor" && ctx.cursor && ctx.monitor && ctx.monitor.x !== undefined) {
      x = ctx.cursor.x - ctx.monitor.x
      y = ctx.cursor.y - ctx.monitor.y
    } else if (mode === "last" && lastPos[mon ? mon.name : ""]) {
      x = lastPos[mon ? mon.name : ""].x
      y = lastPos[mon ? mon.name : ""].y
    }
    const m = Theme.px(8)
    cardX = Math.round(Math.max(m, Math.min(x, w - cw - m)))
    cardY = Math.round(Math.max(m, Math.min(y, h - ch - m)))
  }

  // ---- data ----------------------------------------------------------------

  function refresh(resetSelection) {
    const keepId = currentRow ? currentRow.id : -1
    Daemon.call("list", { query: query, limit: 300 }, (r, err) => {
      if (!r) return
      stale = false
      rows = r.rows
      total = r.total
      let idx = 0
      if (!resetSelection && keepId >= 0) {
        const found = rows.findIndex(x => x.id === keepId)
        if (found >= 0) idx = found
      }
      list.currentIndex = Math.min(idx, Math.max(0, rows.length - 1))
      if (previewOpen) loadDetail()
    })
  }

  function loadDetail() {
    const row = currentRow
    if (!row) { detail = null; return }
    if (detail && detail.id === row.id) return
    Daemon.call("get", { id: row.id }, (d) => { if (d && currentRow && d.id === currentRow.id) detail = d })
  }

  Connections {
    target: Daemon
    function onEvent(name, data) {
      if (name.startsWith("clip") || name === "clips.reset") {
        stale = true
        // While open, follow changes; while closed, refresh quietly so the
        // next open is instant.
        eventRefresh.interval = popup.open ? 60 : 400
        eventRefresh.restart()
        if (name === "clip.deleted" && detail && detail.id === data.id) detail = null
      }
    }
    function onConnectedAgain() { popup.stale = true; eventRefresh.restart() }
  }
  Timer { id: eventRefresh; interval: 400; onTriggered: popup.refresh(false) }
  Timer { id: searchDebounce; interval: 25; onTriggered: popup.refresh(true) }
  Timer { id: detailDebounce; interval: 40; onTriggered: popup.loadDetail() }
  Timer { id: ticker; property real now: Date.now(); interval: 30000; repeat: true; running: popup.open; onTriggered: now = Date.now() }

  // ---- actions -------------------------------------------------------------

  // Ids to act on: the picked rows in list order, or else the current row.
  function targetIds() {
    const ids = rows.filter(r => picked[r.id]).map(r => r.id)
    if (ids.length) return ids
    return currentRow ? [currentRow.id] : []
  }

  function paste(ids, mode) {
    if (!ids.length) return
    hide()
    Daemon.call("paste", { ids: ids, mode: mode || "normal" }, (r, err) => { if (err) notify(err.message) })
  }

  function copyOnly(ids) {
    if (!ids.length) return
    hide()
    Daemon.call("copy", { ids: ids }, (r, err) => { if (err) notify(err.message) })
  }

  function pastePosition(n) {
    if (n < rows.length) paste([rows[n].id], "normal")
  }

  function deleteTargets() {
    const ids = targetIds()
    if (!ids.length) return
    const at = list.currentIndex
    Daemon.call("delete", { ids: ids }, () => {
      picked = {}
      refresh(false)
      list.currentIndex = Math.min(at, Math.max(0, rows.length - 2))
    })
  }

  function move(delta) {
    if (!rows.length) return
    list.currentIndex = Math.max(0, Math.min(rows.length - 1, list.currentIndex + delta))
  }

  function togglePick(index) {
    const r = rows[index]
    if (!r) return
    const next = Object.assign({}, picked)
    if (next[r.id]) delete next[r.id]
    else next[r.id] = true
    picked = next
    pickAnchor = index
  }

  function pickRange(from, to) {
    const next = Object.assign({}, picked)
    for (let i = Math.min(from, to); i <= Math.max(from, to); i++) if (rows[i]) next[rows[i].id] = true
    picked = next
  }

  function notify(msg) {
    notifier.command = ["notify-send", "-a", "CLIP//NET", "CLIP//NET", msg]
    notifier.running = true
  }
  Process { id: notifier }

  onPreviewOpenChanged: if (previewOpen) loadDetail()

  // ---- keys ----------------------------------------------------------------

  function handleKey(e) {
    const ctrl = e.modifiers & Qt.ControlModifier
    const shift = e.modifiers & Qt.ShiftModifier
    const alt = e.modifiers & Qt.AltModifier
    const k = e.key

    if (k === Qt.Key_Escape) {
      if (search.text && Daemon.setting("esc_clears_search", true) && !shift) search.text = ""
      else hide()
    } else if (k === Qt.Key_Return || k === Qt.Key_Enter) {
      paste(targetIds(), shift ? "plain" : "normal")
    } else if (ctrl && k >= Qt.Key_1 && k <= Qt.Key_9) {
      pastePosition(k - Qt.Key_1)
    } else if (ctrl && k === Qt.Key_0) {
      pastePosition(9)
    } else if (k === Qt.Key_Down) {
      if (shift) { if (pickAnchor < 0) pickAnchor = list.currentIndex; move(1); pickRange(pickAnchor, list.currentIndex) }
      else move(1)
    } else if (k === Qt.Key_Up) {
      if (shift) { if (pickAnchor < 0) pickAnchor = list.currentIndex; move(-1); pickRange(pickAnchor, list.currentIndex) }
      else move(-1)
    } else if (previewOpen && shift && (k === Qt.Key_PageDown || k === Qt.Key_PageUp)) {
      pane.scroll(k === Qt.Key_PageDown ? 10 : -10)
    } else if (k === Qt.Key_PageDown) {
      move(visibleRows - 1)
    } else if (k === Qt.Key_PageUp) {
      move(-(visibleRows - 1))
    } else if (ctrl && k === Qt.Key_Home) {
      list.currentIndex = 0
    } else if (ctrl && k === Qt.Key_End) {
      list.currentIndex = Math.max(0, rows.length - 1)
    } else if (k === Qt.Key_F3) {
      previewOpen = !previewOpen
    } else if (previewOpen && alt && k === Qt.Key_W) {
      pane.wrap = !pane.wrap
    } else if (k === Qt.Key_Delete && (!search.text || search.cursorPosition >= search.text.length)) {
      deleteTargets()
    } else if (alt && k === Qt.Key_C) {
      search.text = ""
    } else if (ctrl && k === Qt.Key_C && !search.selectedText) {
      copyOnly(targetIds())
    } else if (ctrl && k === Qt.Key_A && !search.text) {
      pickRange(0, rows.length - 1)
    } else {
      return false
    }
    return true
  }

  // ---- layout --------------------------------------------------------------

  Rectangle {
    anchors.fill: parent
    color: Qt.rgba(Theme.scrim.r, Theme.scrim.g, Theme.scrim.b, Daemon.setting("scrim_alpha", 0) / 100)
  }

  MouseArea {
    anchors.fill: parent
    onClicked: popup.hide()
  }

  Rectangle {
    id: card
    x: popup.cardX
    y: popup.cardY
    width: popup.listWidth + (popup.previewOpen ? popup.paneWidth + popup.pad : 0) + popup.pad * 2
    height: searchBox.height + listBox.height + status.height + popup.pad * 4
    color: Theme.card
    border.color: Theme.border
    border.width: Theme.borderWidth
    radius: Theme.radius

    // Swallow clicks so they do not reach the close-on-outside-click area.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    Rectangle {
      id: searchBox
      x: popup.pad
      y: popup.pad
      width: popup.listWidth
      height: Math.round(Theme.fontSize * 2.3)
      color: Theme.faint
      radius: Math.min(Theme.radius, 6)

      Text {
        id: searchIcon
        anchors.left: parent.left
        anchors.leftMargin: Theme.px(8)
        anchors.verticalCenter: parent.verticalCenter
        text: ""
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
      }

      TextInput {
        id: search
        anchors.left: searchIcon.right
        anchors.right: badges.left
        anchors.leftMargin: Theme.px(8)
        anchors.rightMargin: Theme.px(8)
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.card
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize + 1
        clip: true
        focus: true
        onTextChanged: { popup.query = text; searchDebounce.restart() }
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: e => { if (popup.handleKey(e)) e.accepted = true }

        Text {
          visible: !search.text
          anchors.verticalCenter: parent.verticalCenter
          text: "Search clips…"
          color: Theme.dim
          font: search.font
        }
      }

      Row {
        id: badges
        anchors.right: parent.right
        anchors.rightMargin: Theme.px(8)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.px(6)
        Text {
          visible: Daemon.paused
          text: " PAUSED"
          color: Theme.urgent
          font.family: Theme.fontFamily
          font.pixelSize: Theme.fontSize - 2
        }
      }
    }

    Item {
      id: listBox
      x: popup.pad
      y: searchBox.y + searchBox.height + popup.pad
      width: popup.listWidth
      height: popup.rowHeight * popup.visibleRows

      ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: popup.rows
        boundsBehavior: Flickable.StopAtBounds
        highlightMoveDuration: 0
        highlightFollowsCurrentItem: false
        keyNavigationEnabled: false
        cacheBuffer: popup.rowHeight * 20
        onCurrentIndexChanged: if (popup.previewOpen) detailDebounce.restart()

        delegate: ClipRow {
          current: index === list.currentIndex
          selected: !!popup.picked[modelData.id]
          lineHeight: popup.rowHeight
          now: ticker.now
          showThumbnails: Daemon.setting("show_thumbnails", true)
          onClicked: mouse => {
            if (mouse.modifiers & Qt.ControlModifier) popup.togglePick(index)
            else if (mouse.modifiers & Qt.ShiftModifier) popup.pickRange(popup.pickAnchor < 0 ? list.currentIndex : popup.pickAnchor, index)
            else { popup.picked = {}; popup.pickAnchor = index }
            list.currentIndex = index
          }
          onDoubleClicked: popup.paste([modelData.id], "normal")
        }
      }

      Text {
        anchors.centerIn: parent
        visible: popup.offlineText || !popup.rows.length
        width: parent.width - Theme.px(40)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
        text: popup.offlineText ? popup.offlineText
            : popup.query ? "Nothing matches “" + popup.query + "”"
            : "Nothing copied yet"
      }
    }

    PreviewPane {
      id: pane
      visible: popup.previewOpen
      x: listBox.x + listBox.width + popup.pad
      y: popup.pad
      width: popup.paneWidth
      height: listBox.y + listBox.height - popup.pad
      detail: popup.detail
    }

    // Status bar: where you are, how many, and what is selected.
    Item {
      id: status
      x: popup.pad
      y: listBox.y + listBox.height + popup.pad
      width: card.width - popup.pad * 2
      height: Math.round(Theme.fontSize * 1.4)

      Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
        text: {
          const n = Object.keys(popup.picked).length
          let s = "History · " + (popup.query ? popup.total + " found" : popup.total + " clips")
          if (n) s += " · " + n + " selected"
          return s
        }
      }
      Text {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
        text: {
          const r = popup.currentRow
          if (!r) return ""
          const parts = []
          if (r.source_app) parts.push(r.source_app)
          parts.push(Format.size(r.size))
          return parts.join(" · ")
        }
      }
    }
  }
}

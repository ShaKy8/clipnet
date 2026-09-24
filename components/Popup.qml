import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.common
import "ui"

// The clipboard history popup: Ditto's list, in the Omarchy theme.
//
// A transparent full-screen overlay on the monitor under the cursor, with the
// card placed at the cursor (or centered, or where it was last). Clicking
// outside the card closes it. Keyboard focus is exclusive while open, and
// focus returns to the previous window as soon as the surface unmaps, which
// is what the daemon waits for before it sends the paste keystroke.
//
// Views: History (every clip), Groups (Ctrl+G: the top-level groups), and a
// group (its subgroups, then its clips). Dialogs and menus are drawn inside
// the card and hold the keyboard while they are open.
PanelWindow {
  id: popup

  signal settingsRequested()

  property bool open: false
  property var rows: []          // group rows ({isGroup: true, ...}) then clip rows
  property int groupRows: 0
  property int total: 0
  property bool totalMore: false   // the daemon stopped counting (1000+)
  property string query: ""
  property bool previewOpen: false
  property var detail: null
  property bool stale: true
  property string offlineText: ""
  property var picked: ({})      // clip ids picked for a multi-paste
  property int pickAnchor: -1
  property real cardX: 0
  property real cardY: 0
  property var lastPos: ({})

  // Where we are: History (groupId 0, !groupsRoot), the Groups list, or a group.
  property int groupId: 0
  property bool groupsRoot: false
  property var groups: []
  property var transforms: []

  property Item overlay: null    // the open dialog or menu, if any

  // Ditto's Ctrl+Space: stay open after pasting. The popup comes back once
  // the paste keystroke is done (the daemon's Lua sends clipnet:pasted), so
  // it never takes focus back before the target app has received the keys.
  property bool keepOpen: false
  property bool reopenPending: false
  property var hoverTip: null     // { row, detail, y } while a hover preview shows

  readonly property int rowHeight: Math.round(Theme.fontSize * 2)
  readonly property int visibleRows: Daemon.setting("popup_rows", 14)
  readonly property int listWidth: Theme.px(Daemon.setting("popup_width", 520))
  readonly property int paneWidth: Theme.px(440)
  readonly property int pad: Theme.px(8)
  readonly property var currentRow: rows.length ? rows[Math.max(0, Math.min(list.currentIndex, rows.length - 1))] : null
  readonly property var currentClip: currentRow && !currentRow.isGroup ? currentRow : null

  visible: open
  color: "transparent"
  anchors { top: true; bottom: true; left: true; right: true }
  exclusionMode: ExclusionMode.Ignore
  WlrLayershell.namespace: "clipnet"
  WlrLayershell.layer: WlrLayer.Overlay
  WlrLayershell.keyboardFocus: open ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

  // ---- showing and hiding ------------------------------------------------

  function toggle() { open ? hide() : show() }

  function show(keepState) {
    Daemon.call("show_context", {}, (ctx, err) => {
      if (!keepState) place(ctx || {})
      closeOverlay()
      if (keepState) {
        offlineText = err ? "clipnetd is not running — start it with: clipnet start" : ""
        open = true
        search.forceActiveFocus()
        return
      }
      search.text = ""
      query = ""
      picked = {}
      pickAnchor = -1
      groupId = 0
      groupsRoot = false
      offlineText = err ? "clipnetd is not running — start it with: clipnet start" : ""
      refresh(true)
      loadGroups()
      open = true
      search.forceActiveFocus()
      ticker.now = Date.now()
    })
  }

  // The paste keystroke finished (see keepOpen).
  function pasted() {
    if (!reopenPending) return
    reopenPending = false
    reopenFallback.stop()
    if (keepOpen && !open) show(true)
  }
  Timer { id: reopenFallback; interval: 2000; onTriggered: popup.pasted() }

  function hide() {
    if (!open) return
    hoverTip = null
    closeOverlay()
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

  function childGroups(parent) {
    return groups.filter(g => (g.parent_id || 0) === parent).map(g => Object.assign({ isGroup: true }, g))
  }

  function refresh(resetSelection) {
    const keepId = currentRow ? (currentRow.isGroup ? "g" : "c") + currentRow.id : ""
    const gRows = groupsRoot ? childGroups(0) : groupId ? childGroups(groupId) : []
    const done = (clips, count, more) => {
      stale = false
      rows = gRows.concat(clips)
      groupRows = gRows.length
      total = count
      totalMore = !!more
      let idx = 0
      if (!resetSelection && keepId) {
        const found = rows.findIndex(x => (x.isGroup ? "g" : "c") + x.id === keepId)
        if (found >= 0) idx = found
      }
      list.currentIndex = Math.min(idx, Math.max(0, rows.length - 1))
      if (previewOpen) loadDetail()
    }
    if (groupsRoot && !query) { done([], 0); return }
    // Searching in the Groups list looks through every clip.
    const args = { query: query, limit: 300 }
    if (groupId && !groupsRoot) args.group = groupId
    Daemon.call("list", args, (r, err) => {
      if (r) { offlineText = ""; done(r.rows, r.total, r.more); return }
      // Never leave an old list standing in for results we could not get.
      if (err && err.code === "offline") {
        offlineText = "clipnetd is not running — start it with: clipnet start"
        rows = []
        total = 0
      }
    })
  }

  function loadGroups(then) {
    Daemon.call("groups.list", {}, (g) => {
      if (g) groups = g
      if (then) then()
    })
  }

  function loadDetail() {
    const row = currentClip
    if (!row) { detail = null; return }
    if (detail && detail.id === row.id) return
    Daemon.call("get", { id: row.id }, (d) => { if (d && currentClip && d.id === currentClip.id) detail = d })
  }

  function groupPath(id) {
    const byId = {}
    for (const g of groups) byId[g.id] = g
    const parts = []
    for (let g = byId[id], n = 0; g && n < 64; g = byId[g.parent_id], n++) parts.unshift(g.name)
    return parts
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
      } else if (name === "groups.changed") {
        popup.loadGroups(() => { if (popup.open) eventRefresh.restart() })
      }
    }
    function onConnectedAgain() {
      popup.offlineText = ""
      popup.stale = true
      eventRefresh.restart()
      popup.loadGroups()
      Daemon.call("transforms.list", {}, t => { if (t) popup.transforms = t })
    }
  }
  Timer { id: eventRefresh; interval: 400; onTriggered: popup.refresh(false) }
  Timer { id: searchDebounce; interval: 25; onTriggered: popup.refresh(true) }
  Timer { id: detailDebounce; interval: 40; onTriggered: popup.loadDetail() }
  Timer { id: ticker; property real now: Date.now(); interval: 30000; repeat: true; running: popup.open; onTriggered: now = Date.now() }

  // ---- navigation ----------------------------------------------------------

  function enterGroup(id) {
    groupId = id
    groupsRoot = false
    picked = {}
    search.text = ""
    query = ""
    refresh(true)
  }

  function up() {
    if (groupId) {
      const g = groups.find(x => x.id === groupId)
      const parent = g ? (g.parent_id || 0) : 0
      const from = groupId
      if (parent) enterGroup(parent)
      else { groupId = 0; groupsRoot = true; refresh(true) }
      // Land on the group we came out of.
      Qt.callLater(() => {
        const i = rows.findIndex(r => r.isGroup && r.id === from)
        if (i >= 0) list.currentIndex = i
      })
    } else if (groupsRoot) {
      groupsRoot = false
      refresh(true)
    }
  }

  function toggleGroups() {
    picked = {}
    if (groupsRoot || groupId) { groupsRoot = false; groupId = 0 }
    else groupsRoot = true
    search.text = ""
    query = ""
    refresh(true)
  }

  // ---- actions -------------------------------------------------------------

  // Clip ids to act on: the picked clips in list order, or the current clip.
  function targetIds() {
    const ids = rows.filter(r => !r.isGroup && picked[r.id]).map(r => r.id)
    if (ids.length) return ids
    return currentClip ? [currentClip.id] : []
  }

  function report(r, err) { if (err) notify(err.message) }

  function paste(ids, mode, transform) {
    if (!ids.length) return
    if (keepOpen) { reopenPending = true; reopenFallback.restart() }
    hide()
    const args = { ids: ids, mode: mode || "normal" }
    if (transform) args.transform = transform
    Daemon.call("paste", args, report)
  }

  function copyOnly(ids) {
    if (!ids.length) return
    hide()
    Daemon.call("copy", { ids: ids }, report)
  }

  function activate(mode) {
    const r = currentRow
    if (r && r.isGroup && !Object.keys(picked).length) { enterGroup(r.id); return }
    paste(targetIds(), mode)
  }

  // Ctrl+1..0 count clips only: group rows above them are not numbered.
  function pastePosition(n) {
    const i = groupRows + n
    if (i < rows.length) paste([rows[i].id], "normal")
  }

  function deleteTargets() {
    const r = currentRow
    if (r && r.isGroup && !Object.keys(picked).length) { confirmDeleteGroup(r); return }
    const ids = targetIds()
    if (!ids.length) return
    const run = () => {
      const at = list.currentIndex
      Daemon.call("delete", { ids: ids }, () => {
        picked = {}
        refresh(false)
        list.currentIndex = Math.min(at, Math.max(0, rows.length - 2))
      })
    }
    if (ids.length > 1) {
      openOverlay(confirmComp, { title: "Delete clips", message: "Delete " + ids.length + " clips?", okText: "Delete", danger: true },
                  o => { o.confirmed.connect(() => { closeOverlay(); run() }) })
    } else run()
  }

  function toggleSticky() {
    const c = currentClip
    if (!c) return
    Daemon.call("update", { id: c.id, sticky: c.sticky ? false : "top" }, report)
  }

  function toggleLock() {
    const c = currentClip
    if (!c) return
    Daemon.call("update", { id: c.id, locked: !c.locked }, report)
  }

  // Alt+↑/↓: move a sticky clip among the sticky clips.
  function moveSticky(dir) {
    const c = currentClip
    if (!c || !c.sticky) return
    const sticky = rows.filter(r => !r.isGroup && r.sticky).map(r => r.id)
    const i = sticky.indexOf(c.id), j = i + dir
    if (i < 0 || j < 0 || j >= sticky.length) return
    sticky[i] = sticky[j]
    sticky[j] = c.id
    Daemon.call("reorder_sticky", { ids: sticky }, report)
    list.currentIndex = Math.max(0, Math.min(rows.length - 1, list.currentIndex + dir))
  }

  function newGroup(fromSelection) {
    const ids = fromSelection ? targetIds() : []
    const parent = groupsRoot ? 0 : groupId
    openOverlay(promptComp, { title: fromSelection ? "New group from " + ids.length + (ids.length === 1 ? " clip" : " clips") : "New group",
                              label: parent ? "Inside " + groupPath(parent).join(" ▸ ") : "Name", okText: "Create" }, o => {
      o.accepted.connect(name => {
        Daemon.call("groups.create", { name: name, parent: parent }, (r, err) => {
          if (err) { o.error = err.message; return }
          closeOverlay()
          if (ids.length) Daemon.call("move", { ids: ids, group: r.id }, report)
          picked = {}
        })
      })
    })
  }

  function renameGroup(g) {
    openOverlay(promptComp, { title: "Rename group", label: "Name", initial: g.name, okText: "Rename" }, o => {
      o.accepted.connect(name => {
        Daemon.call("groups.rename", { id: g.id, name: name }, (r, err) => {
          if (err) { o.error = err.message; return }
          closeOverlay()
        })
      })
    })
  }

  function confirmDeleteGroup(g) {
    const sub = groups.filter(x => x.parent_id === g.id).length
    openOverlay(confirmComp, {
      title: "Delete group “" + g.name + "”",
      message: "Its clips" + (sub ? " (and those in its " + sub + " subgroup" + (sub > 1 ? "s" : "") + ")" : "") +
               " go back to plain history. Or delete them too.",
      okText: "Delete group", altText: "Delete clips too", danger: true,
    }, o => {
      const run = cascade => Daemon.call("groups.delete", { id: g.id, cascade: cascade }, (r, err) => { closeOverlay(); report(r, err) })
      o.confirmed.connect(() => run(false))
      o.alternative.connect(() => run(true))
    })
  }

  function moveTo(groupTarget) {
    const ids = targetIds()
    if (!ids.length) return
    Daemon.call("move", { ids: ids, group: groupTarget }, (r, err) => { picked = {}; report(r, err) })
  }

  function editClip(isNew) {
    const open = (clip, text) => openOverlay(editorComp, { clip: clip, initialText: text }, o => {
      o.saved.connect(t => {
        if (clip) {
          Daemon.call("set_text", { id: clip.id, text: t }, (r, err) => { if (err) o.error = err.message; else closeOverlay() })
        } else {
          Daemon.call("create", { text: t }, (r, err) => {
            if (err) { o.error = err.message; return }
            if (groupId && !groupsRoot) Daemon.call("move", { ids: [r.id], group: groupId }, report)
            closeOverlay()
          })
        }
      })
    })
    if (isNew) { open(null, ""); return }
    const c = currentClip
    if (!c) return
    Daemon.call("get", { id: c.id }, (d, err) => {
      if (!d) return report(null, err)
      if (d.text === null) { notify("This clip has no text to edit."); return }
      open(d, d.text)
    })
  }

  // Ctrl+F2: the two picked clips, or one picked clip and the current one.
  function compareClips() {
    let ids = rows.filter(r => !r.isGroup && picked[r.id]).map(r => r.id)
    if (ids.length === 1 && currentClip && currentClip.id !== ids[0]) ids.push(currentClip.id)
    if (ids.length !== 2) { notify("Pick two clips to compare: Ctrl+click them, then Ctrl+F2."); return }
    Daemon.call("get", { id: ids[0] }, (a, err) => {
      if (!a) return report(null, err)
      Daemon.call("get", { id: ids[1] }, (b, err2) => {
        if (!b) return report(null, err2)
        if (a.text === null || b.text === null) { notify("Only clips with text can be compared."); return }
        openOverlay(compareComp, { leftClip: a, rightClip: b })
      })
    })
  }

  function properties() {
    const c = currentClip
    if (!c) return
    openOverlay(propsComp, { clip: c, groups: groups }, o => {
      o.save.connect(f => applyProperties(c, f, o))
    })
  }

  // Apply the properties dialog's fields one request at a time, stopping at
  // the first refusal (a taken quick-paste word or hotkey) with its message.
  function applyProperties(c, f, dialog) {
    const steps = []
    const upd = {}
    if ((c.title || "") !== f.title.trim()) upd.title = f.title
    if ((c.quick_paste || "") !== f.quick_paste.trim()) upd.quick_paste = f.quick_paste
    if (c.locked !== f.locked) upd.locked = f.locked
    if (c.sticky !== f.sticky) upd.sticky = f.sticky ? "top" : false
    if (Object.keys(upd).length) steps.push(cb => Daemon.call("update", Object.assign({ id: c.id }, upd), cb))
    if ((c.group_id || 0) !== f.group) steps.push(cb => Daemon.call("move", { ids: [c.id], group: f.group }, cb))
    if ((c.hotkey || "") !== f.hotkey) {
      steps.push(cb => Daemon.call("hotkeys.list", {}, (list, err) => {
        if (err) return cb(null, err)
        const existing = list.find(h => h.action === "paste_clip" && h.arg === c.uuid)
        if (!f.hotkey) {
          if (existing) Daemon.call("hotkeys.remove", { id: existing.id }, cb)
          else cb(null, null)
        } else {
          Daemon.call("hotkeys.set", { id: existing ? existing.id : 0, accel: f.hotkey, action: "paste_clip", arg: c.uuid }, cb)
        }
      }))
    }
    const next = i => {
      if (i >= steps.length) { closeOverlay(); return }
      steps[i]((r, err) => { if (err) dialog.error = err.message; else next(i + 1) })
    }
    next(0)
  }

  function notify(msg) {
    notifier.command = ["notify-send", "-a", "CLIP//NET", "CLIP//NET", msg]
    notifier.running = true
  }
  Process { id: notifier }

  onPreviewOpenChanged: if (previewOpen) loadDetail()

  // ---- hover preview -------------------------------------------------------
  // Resting on a row for a moment shows more of it: the full text, or the
  // image at a readable size. Any key, click or scroll hides it.

  property Item hoverItem: null
  property var hoverClip: null
  function hoverRow(item, clip) {
    hoverItem = item
    hoverClip = clip
    hoverTip = null
    if (item && Daemon.setting("preview_on_hover", true) && !previewOpen && !overlay) hoverTimer.restart()
    else hoverTimer.stop()
  }
  Timer {
    id: hoverTimer
    interval: 600
    onTriggered: {
      const item = popup.hoverItem, clip = popup.hoverClip
      if (!item || !clip) return
      Daemon.call("get", { id: clip.id }, d => {
        if (!d || popup.hoverClip !== clip || !popup.hoverItem) return
        const p = item.mapToItem(card, 0, item.height)
        popup.hoverTip = { detail: d, y: p.y }
      })
    }
  }
  Connections { target: list; function onContentYChanged() { popup.hoverTip = null; hoverTimer.stop() } }

  // ---- dialogs and menus ---------------------------------------------------

  Component { id: confirmComp; Confirm {} }
  Component { id: promptComp; Prompt {} }
  Component { id: editorComp; ClipEditor {} }
  Component { id: propsComp; Properties {} }
  Component { id: menuComp; Menu {} }
  Component { id: compareComp; CompareView {} }

  function openOverlay(comp, props, wire) {
    closeOverlay()
    const o = comp.createObject(card, props)
    if (!o) { console.warn("clipnet: could not open a dialog"); return }
    if (o.cancelled) o.cancelled.connect(closeOverlay)
    if (o.closed) o.closed.connect(closeOverlay)
    if (wire) wire(o)
    overlay = o
    if (o.focusMenu) o.focusMenu()
  }

  function closeOverlay() {
    if (!overlay) return
    const o = overlay
    overlay = null
    o.destroy()
    if (open) search.forceActiveFocus()
  }

  function menuAt(items, x, y) {
    openOverlay(menuComp, { items: items, px: x, py: y }, o => {
      o.triggered.connect((action, data) => { closeOverlay(); runAction(action, data) })
    })
  }

  function specialPasteItems() {
    return transforms.map(t => ({ label: t.label, action: "transform", data: t.id }))
  }

  function groupItems() {
    const items = [{ label: "History only (no group)", action: "move", data: 0 }]
    for (const g of groups) items.push({ label: groupPath(g.id).join(" ▸ "), action: "move", data: g.id })
    items.push({ separator: true })
    items.push({ label: "New group from selection…", action: "group-from-selection", shortcut: "F7" })
    return items
  }

  // The context menu for the current row (Menu key, Shift+F10, right-click).
  function contextMenu(x, y) {
    const r = currentRow
    if (!r) return
    if (r.isGroup) {
      menuAt([
        { label: "Open", action: "open", shortcut: "Enter" },
        { label: "Rename…", action: "rename", shortcut: "F2" },
        { label: "New group inside…", action: "new-group-in" },
        { separator: true },
        { label: "Delete group…", action: "delete", shortcut: "Del" },
      ], x, y)
      return
    }
    const n = targetIds().length
    const one = n === 1
    menuAt([
      { label: n > 1 ? "Paste " + n + " clips" : "Paste", action: "paste", shortcut: "Enter" },
      { label: "Paste as plain text", action: "paste-plain", shortcut: "Shift+Enter" },
      { label: "Special Paste", submenu: specialPasteItems() },
      { label: "Copy (don't paste)", action: "copy", shortcut: "Ctrl+C" },
      { label: "Compare two clips", action: "compare", shortcut: "Ctrl+F2", enabled: Object.keys(picked).length >= 1 },
      { separator: true },
      { label: "Edit…", action: "edit", shortcut: "Ctrl+E", enabled: one },
      { label: "Properties…", action: "properties", shortcut: "Alt+Enter", enabled: one },
      { label: currentClip.sticky ? "Unstick" : "Make sticky", action: "sticky", shortcut: "Ctrl+S", enabled: one },
      { label: currentClip.locked ? "Allow auto-delete" : "Never auto-delete", action: "lock", shortcut: "Ctrl+L", enabled: one },
      { label: "Move to group", submenu: groupItems() },
      { separator: true },
      { label: "New clip…", action: "new", shortcut: "Ctrl+N" },
      { label: "Delete", action: "delete", shortcut: "Del" },
    ], x, y)
  }

  function contextMenuAtCurrent() {
    const item = list.itemAtIndex(list.currentIndex)
    const p = item ? item.mapToItem(card, Theme.px(40), item.height) : Qt.point(listBox.x + Theme.px(40), listBox.y)
    contextMenu(p.x, p.y)
  }

  function runAction(action, data) {
    switch (action) {
    case "paste": activate("normal"); break
    case "paste-plain": paste(targetIds(), "plain"); break
    case "transform": paste(targetIds(), "normal", data); break
    case "copy": copyOnly(targetIds()); break
    case "compare": compareClips(); break
    case "edit": editClip(false); break
    case "new": editClip(true); break
    case "properties": properties(); break
    case "sticky": toggleSticky(); break
    case "lock": toggleLock(); break
    case "move": moveTo(data); break
    case "group-from-selection": newGroup(true); break
    case "open": if (currentRow && currentRow.isGroup) enterGroup(currentRow.id); break
    case "rename": if (currentRow && currentRow.isGroup) renameGroup(currentRow); break
    case "new-group-in":
      if (currentRow && currentRow.isGroup) { enterGroup(currentRow.id); Qt.callLater(() => newGroup(false)) }
      break
    case "delete": deleteTargets(); break
    }
  }

  // ---- keys ----------------------------------------------------------------

  function move(delta) {
    if (!rows.length) return
    list.currentIndex = Math.max(0, Math.min(rows.length - 1, list.currentIndex + delta))
  }

  function togglePick(index) {
    const r = rows[index]
    if (!r || r.isGroup) return
    const next = Object.assign({}, picked)
    if (next[r.id]) delete next[r.id]
    else next[r.id] = true
    picked = next
    pickAnchor = index
  }

  function pickRange(from, to) {
    const next = Object.assign({}, picked)
    for (let i = Math.min(from, to); i <= Math.max(from, to); i++) if (rows[i] && !rows[i].isGroup) next[rows[i].id] = true
    picked = next
  }

  function handleKey(e) {
    hoverTip = null
    hoverTimer.stop()
    const ctrl = e.modifiers & Qt.ControlModifier
    const shift = e.modifiers & Qt.ShiftModifier
    const alt = e.modifiers & Qt.AltModifier
    const k = e.key

    if (k === Qt.Key_Escape) {
      if (search.text && Daemon.setting("esc_clears_search", true) && !shift) search.text = ""
      else hide()
    } else if (alt && (k === Qt.Key_Return || k === Qt.Key_Enter)) {
      properties()
    } else if (k === Qt.Key_Return || k === Qt.Key_Enter) {
      activate(shift ? "plain" : "normal")
    } else if (ctrl && !shift && k >= Qt.Key_1 && k <= Qt.Key_9) {
      pastePosition(k - Qt.Key_1)
    } else if (ctrl && !shift && k === Qt.Key_0) {
      pastePosition(9)
    } else if (alt && (k === Qt.Key_Up || k === Qt.Key_Down)) {
      moveSticky(k === Qt.Key_Up ? -1 : 1)
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
    } else if (k === Qt.Key_Backspace && !search.text && (groupId || groupsRoot)) {
      up()
    } else if (ctrl && k === Qt.Key_G) {
      toggleGroups()
    } else if (k === Qt.Key_F7) {
      newGroup(!ctrl)
    } else if (k === Qt.Key_F2 && currentRow && currentRow.isGroup) {
      renameGroup(currentRow)
    } else if (ctrl && k === Qt.Key_E) {
      editClip(false)
    } else if (ctrl && k === Qt.Key_N) {
      editClip(true)
    } else if (ctrl && k === Qt.Key_S) {
      toggleSticky()
    } else if (ctrl && k === Qt.Key_L) {
      toggleLock()
    } else if (ctrl && shift && k === Qt.Key_V) {
      const item = list.itemAtIndex(list.currentIndex)
      const p = item ? item.mapToItem(card, Theme.px(40), item.height) : Qt.point(listBox.x, listBox.y)
      if (targetIds().length) menuAt(specialPasteItems(), p.x, p.y)
    } else if (k === Qt.Key_Menu || (shift && k === Qt.Key_F10)) {
      contextMenuAtCurrent()
    } else if (ctrl && k === Qt.Key_Space) {
      keepOpen = !keepOpen
    } else if (ctrl && k === Qt.Key_F2) {
      compareClips()
    } else if (ctrl && k === Qt.Key_Comma) {
      hide()
      settingsRequested()
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
    // A wide dialog (Compare) may push the card left to stay on screen.
    x: Math.max(Theme.px(8), Math.min(popup.cardX, popup.width - width - Theme.px(8)))
    y: popup.cardY
    width: Math.max(popup.listWidth + (popup.previewOpen ? popup.paneWidth + popup.pad : 0) + popup.pad * 2,
                    popup.overlay && popup.overlay.wantWidth ? Math.min(popup.overlay.wantWidth, popup.width - Theme.px(16)) : 0)
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
          text: popup.groupsRoot ? "Search all clips…" : popup.groupId ? "Search this group…" : "Search clips…"
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
          visible: popup.keepOpen
          text: "\uf08d KEEP OPEN"
          color: Theme.accent
          font.family: Theme.fontFamily
          font.pixelSize: Theme.fontSize - 2
        }
        Text {
          visible: Daemon.paused
          text: Daemon.pausedUntil > 0 ? " PAUSED " + Math.max(1, Math.ceil((Daemon.pausedUntil - ticker.now) / 60000)) + " min"
                                       : " PAUSED"
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
        onCurrentIndexChanged: {
          positionViewAtIndex(currentIndex, ListView.Contain)
          if (popup.previewOpen) detailDebounce.restart()
        }

        delegate: Loader {
          id: rowLoader
          required property int index
          required property var modelData
          width: list.width
          sourceComponent: modelData.isGroup ? groupRow : clipRow

          Component {
            id: groupRow
            GroupRow {
              width: list.width
              index: rowLoader.index
              modelData: rowLoader.modelData
              current: rowLoader.index === list.currentIndex
              lineHeight: popup.rowHeight
              onClicked: list.currentIndex = rowLoader.index
              onDoubleClicked: popup.enterGroup(rowLoader.modelData.id)
              onRightClicked: {
                list.currentIndex = rowLoader.index
                const p = mapToItem(card, Theme.px(40), height)
                popup.contextMenu(p.x, p.y)
              }
            }
          }
          Component {
            id: clipRow
            ClipRow {
              width: list.width
              index: rowLoader.index
              modelData: rowLoader.modelData
              position: rowLoader.index - popup.groupRows
              current: rowLoader.index === list.currentIndex
              selected: !!popup.picked[rowLoader.modelData.id]
              lineHeight: popup.rowHeight
              now: ticker.now
              showThumbnails: Daemon.setting("show_thumbnails", true)
              onClicked: mouse => {
                if (mouse.modifiers & Qt.ControlModifier) popup.togglePick(rowLoader.index)
                else if (mouse.modifiers & Qt.ShiftModifier) popup.pickRange(popup.pickAnchor < 0 ? list.currentIndex : popup.pickAnchor, rowLoader.index)
                else { popup.picked = {}; popup.pickAnchor = rowLoader.index }
                list.currentIndex = rowLoader.index
              }
              onDoubleClicked: popup.paste([rowLoader.modelData.id], "normal")
              onHoverChanged: h => popup.hoverRow(h ? this : null, rowLoader.modelData)
              onRightClicked: {
                if (!popup.picked[rowLoader.modelData.id]) popup.picked = {}
                list.currentIndex = rowLoader.index
                const p = mapToItem(card, Theme.px(40), height)
                popup.contextMenu(p.x, p.y)
              }
            }
          }
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
            : popup.groupsRoot ? "No groups yet. F7 makes one from the selected clips, Ctrl+F7 an empty one."
            : popup.groupId ? "This group is empty. Move clips here with the right-click menu."
            : "Nothing copied yet"
      }
    }

    Rectangle {
      id: tip
      readonly property var d: popup.hoverTip ? popup.hoverTip.detail : null
      readonly property bool isImage: !!d && d.kind === "image" && !!d.image
      visible: !!popup.hoverTip && !popup.overlay
      z: 40
      x: listBox.x + Theme.px(28)
      width: listBox.width - Theme.px(36)
      height: Math.min(isImage ? Theme.px(260) : tipText.implicitHeight + Theme.px(16), Theme.px(300))
      // Below the row, or above it when there is no room.
      y: {
        if (!popup.hoverTip) return 0
        const below = popup.hoverTip.y + Theme.px(2)
        return below + height <= card.height - Theme.px(8) ? below : Math.max(Theme.px(8), popup.hoverTip.y - popup.rowHeight - height - Theme.px(2))
      }
      color: Theme.card
      radius: Math.min(Theme.radius, 6)
      border.width: 1
      border.color: Theme.accent
      clip: true
      Text {
        id: tipText
        visible: !tip.isImage
        x: Theme.px(8)
        y: Theme.px(8)
        width: parent.width - Theme.px(16)
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        textFormat: Text.PlainText
        maximumLineCount: 16
        elide: Text.ElideRight
        text: !tip.d ? "" : (tip.d.text || tip.d.preview || "").substring(0, 4000)
        color: Theme.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 1
      }
      Image {
        visible: tip.isImage
        anchors.fill: parent
        anchors.margins: Theme.px(6)
        source: tip.isImage ? "file://" + tip.d.image : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
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
        anchors.right: detailText.left
        anchors.rightMargin: Theme.px(12)
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideLeft
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
        text: {
          const where = popup.groupId ? ["Groups"].concat(popup.groupPath(popup.groupId)).join(" ▸ ")
                      : popup.groupsRoot ? "Groups" : "History"
          const n = Object.keys(popup.picked).length
          let s = where
          if (!(popup.groupsRoot && !popup.query))
            s += " · " + (popup.query ? popup.total + (popup.totalMore ? "+" : "") + " found"
                                      : popup.total + (popup.total === 1 ? " clip" : " clips"))
          if (n) s += " · " + n + " selected"
          return s
        }
      }
      Text {
        id: detailText
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize - 2
        text: {
          const r = popup.currentClip
          if (!r) return popup.currentRow ? "Enter opens · Backspace goes back" : ""
          const parts = []
          if (r.source_app) parts.push(r.source_app)
          parts.push(Format.size(r.size))
          return parts.join(" · ")
        }
      }
    }
  }
}

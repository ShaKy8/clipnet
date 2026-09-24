import QtQuick
import qs.common

// A keyboard-first menu drawn inside the popup card.
//
// items: [{ label, shortcut?, action?, data?, enabled?, checked?, submenu?: [...] }
//         | { separator: true }]
// ↑↓ move, → or Enter opens a submenu, ← or Esc closes one, Enter triggers,
// a letter jumps to the next item starting with it.
Item {
  id: menu

  property var items: []
  property int current: firstEnabled(0, 1)
  property real px: 0   // where to open, in the parent's coordinates
  property real py: 0
  property Item child: null
  signal triggered(string action, var data)
  signal closed()

  anchors.fill: parent
  z: 60

  readonly property int rowH: Math.round(Theme.fontSize * 1.9)

  function enabledAt(i) {
    const it = items[i]
    return it && !it.separator && it.enabled !== false
  }
  function firstEnabled(from, step) {
    for (let i = from; i >= 0 && i < items.length; i += step) if (enabledAt(i)) return i
    return -1
  }
  function step(dir) {
    let i = current
    for (let n = 0; n < items.length; n++) {
      i = (i + dir + items.length) % items.length
      if (enabledAt(i)) { current = i; return }
    }
  }
  function activate(i) {
    const it = items[i]
    if (!enabledAt(i)) return
    if (it.submenu) { openSub(i); return }
    menu.triggered(it.action || "", it.data)
  }
  function openSub(i) {
    const it = items[i]
    if (!it.submenu || !it.submenu.length) return
    const row = list.itemAtIndex(i)
    const p = row ? row.mapToItem(menu, row.width, 0) : Qt.point(panel.x + panel.width, panel.y)
    // Created dynamically: a static Menu inside Menu.qml would recurse.
    const comp = Qt.createComponent(Qt.resolvedUrl("Menu.qml"))
    child = comp.createObject(menu.parent, { items: it.submenu, px: p.x - Theme.px(4), py: p.y })
    if (!child) { console.warn("clipnet: submenu:", comp.errorString()); return }
    child.triggered.connect((a, d) => menu.triggered(a, d))
    child.closed.connect(() => { child.destroy(); child = null; keys.forceActiveFocus() })
    child.focusMenu()
  }
  function focusMenu() { keys.forceActiveFocus() }

  Component.onDestruction: if (child) child.destroy()

  // Click outside: close the whole menu.
  MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onPressed: menu.closed() }

  Rectangle {
    id: panel
    width: Math.max(Theme.px(220), list.contentWidthHint + Theme.px(24))
    height: Math.min(list.contentHeight + Theme.px(8), menu.height - Theme.px(8))
    x: Math.max(Theme.px(4), Math.min(menu.px, menu.width - width - Theme.px(4)))
    y: Math.max(Theme.px(4), Math.min(menu.py, menu.height - height - Theme.px(4)))
    color: Theme.card
    radius: Math.min(Theme.radius, 8)
    border.width: 1
    border.color: Theme.accent
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    ListView {
      id: list
      property real contentWidthHint: 0
      anchors.fill: parent
      anchors.margins: Theme.px(4)
      clip: true
      interactive: contentHeight > height
      model: menu.items
      currentIndex: menu.current
      delegate: Item {
        required property int index
        required property var modelData
        width: list.width
        height: modelData.separator ? Theme.px(9) : menu.rowH
        Rectangle {
          visible: !!modelData.separator
          anchors.verticalCenter: parent.verticalCenter
          width: parent.width
          height: 1
          color: Theme.faint
        }
        Rectangle {
          visible: !modelData.separator
          anchors.fill: parent
          radius: Math.min(Theme.radius, 4)
          color: index === menu.current ? Theme.selectedBackground : "transparent"
        }
        Row {
          visible: !modelData.separator
          anchors.fill: parent
          anchors.leftMargin: Theme.px(8)
          anchors.rightMargin: Theme.px(8)
          spacing: Theme.px(8)
          Text {
            width: Theme.px(12)
            anchors.verticalCenter: parent.verticalCenter
            text: modelData.checked ? "" : ""
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 2
          }
          Text {
            id: lbl
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - x - sc.width - Theme.px(8)
            elide: Text.ElideRight
            text: modelData.label || ""
            color: modelData.enabled === false ? Theme.dim : index === menu.current ? Theme.selectedText : Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
            Component.onCompleted: list.contentWidthHint = Math.max(list.contentWidthHint, implicitWidth + Theme.px(140))
          }
          Text {
            id: sc
            anchors.verticalCenter: parent.verticalCenter
            text: modelData.submenu ? "▸" : (modelData.shortcut || "")
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 2
          }
        }
        MouseArea {
          anchors.fill: parent
          hoverEnabled: true
          enabled: !modelData.separator
          onEntered: if (menu.enabledAt(index)) menu.current = index
          onClicked: menu.activate(index)
        }
      }
    }
  }

  Item {
    id: keys
    focus: true
    Keys.onPressed: e => {
      const k = e.key
      if (k === Qt.Key_Down || k === Qt.Key_Tab) menu.step(1)
      else if (k === Qt.Key_Up || k === Qt.Key_Backtab) menu.step(-1)
      else if (k === Qt.Key_Home) menu.current = menu.firstEnabled(0, 1)
      else if (k === Qt.Key_End) menu.current = menu.firstEnabled(menu.items.length - 1, -1)
      else if (k === Qt.Key_Return || k === Qt.Key_Enter || k === Qt.Key_Space) menu.activate(menu.current)
      else if (k === Qt.Key_Right) menu.openSub(menu.current)
      else if (k === Qt.Key_Escape || k === Qt.Key_Left || k === Qt.Key_Menu) menu.closed()
      else if (e.text && e.text.length === 1 && !(e.modifiers & (Qt.ControlModifier | Qt.AltModifier))) {
        const ch = e.text.toLowerCase()
        for (let n = 1; n <= menu.items.length; n++) {
          const i = (menu.current + n) % menu.items.length
          if (menu.enabledAt(i) && String(menu.items[i].label).toLowerCase().startsWith(ch)) { menu.current = i; break }
        }
      } else return
      e.accepted = true
    }
  }
}

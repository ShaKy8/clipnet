import QtQuick
import qs.common
import "ui"

// A clip's properties (Ditto's Alt+Enter): description, group, hotkey,
// quick-paste word, sticky and never-delete. Tab moves between fields,
// Ctrl+S or Enter in a text field saves, Esc cancels.
Sheet {
  id: pr

  property var clip: null
  property var groups: []
  property int groupId: clip && clip.group_id ? clip.group_id : 0
  property string hotkey: clip && clip.hotkey ? clip.hotkey : ""
  property string error: ""
  // (fields, done): the owner applies them and calls done(errorMessage | "").
  signal save(var fields)
  signal cancelled()

  title: "Clip properties"
  panelWidth: Theme.px(520)
  onCloseRequested: cancelled()
  Component.onCompleted: { titleField.text = clip.title || ""; wordField.text = clip.quick_paste || ""; titleField.focusField() }

  function submit() {
    save({
      title: titleField.text,
      quick_paste: wordField.text,
      group: groupId,
      hotkey: hotkey,
      sticky: stickyToggle.checked,
      locked: lockToggle.checked,
    })
  }

  function groupPath(id) {
    const byId = {}
    for (const g of groups) byId[g.id] = g
    const parts = []
    for (let g = byId[id], n = 0; g && n < 64; g = byId[g.parent_id], n++) parts.unshift(g.name)
    return parts.join(" ▸ ")
  }

  Field {
    id: titleField
    width: parent.width
    label: "Description (shown instead of the text)"
    placeholder: pr.clip ? pr.clip.preview : ""
    onAccepted: pr.submit()
  }
  Field {
    id: wordField
    width: parent.width
    label: "Quick-paste word (type it in the search, press Enter)"
    placeholder: "e.g. sig"
    onAccepted: pr.submit()
  }
  HotkeyField {
    width: parent.width
    label: "Hotkey (pastes this clip from anywhere)"
    accel: pr.hotkey
    onChanged: a => pr.hotkey = a
  }
  Column {
    width: parent.width
    spacing: Theme.px(4)
    Text {
      text: "Group"
      color: Theme.dim
      font.family: Theme.fontFamily
      font.pixelSize: Theme.fontSize - 2
    }
    Rectangle {
      id: groupBox
      width: parent.width
      height: Math.round(Theme.fontSize * 2.2)
      radius: Math.min(Theme.radius, 6)
      color: Theme.faint
      activeFocusOnTab: true
      border.width: activeFocus ? 2 : 1
      border.color: activeFocus ? Theme.accent : Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.2)
      Text {
        anchors.left: parent.left
        anchors.leftMargin: Theme.px(8)
        anchors.verticalCenter: parent.verticalCenter
        text: pr.groupId ? pr.groupPath(pr.groupId) : "None (history only)"
        color: pr.groupId ? Theme.text : Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
      }
      Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.px(8)
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: Theme.dim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize
      }
      function pick() {
        const items = [{ label: "None (history only)", action: "group", data: 0, checked: !pr.groupId }]
        for (const g of pr.groups) items.push({ label: pr.groupPath(g.id), action: "group", data: g.id, checked: pr.groupId === g.id })
        const m = menuComp.createObject(pr, { items: items, px: groupBox.mapToItem(pr, 0, groupBox.height).x,
                                              py: groupBox.mapToItem(pr, 0, groupBox.height).y })
        m.triggered.connect((a, d) => { pr.groupId = d; m.destroy(); groupBox.forceActiveFocus() })
        m.closed.connect(() => { m.destroy(); groupBox.forceActiveFocus() })
        m.focusMenu()
      }
      MouseArea { anchors.fill: parent; onClicked: groupBox.pick() }
      Keys.onPressed: e => {
        if (e.key === Qt.Key_Space || e.key === Qt.Key_Return || e.key === Qt.Key_Down) { pick(); e.accepted = true }
      }
    }
  }
  Toggle { id: stickyToggle; width: parent.width; text: "Sticky (always at the top)"; checked: !!pr.clip && pr.clip.sticky }
  Toggle { id: lockToggle; width: parent.width; text: "Never delete automatically"; checked: !!pr.clip && pr.clip.locked }
  Text {
    visible: !!pr.error
    width: parent.width
    wrapMode: Text.Wrap
    text: pr.error
    color: Theme.urgent
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize - 1
  }
  Row {
    anchors.right: parent.right
    spacing: Theme.px(8)
    Btn { text: "Cancel"; onClicked: pr.cancelled() }
    Btn { text: "Save"; primary: true; onClicked: pr.submit() }
  }

  Component { id: menuComp; Menu {} }

  Keys.onPressed: e => {
    if (e.key === Qt.Key_Escape) { pr.cancelled(); e.accepted = true }
    else if ((e.modifiers & Qt.ControlModifier) && e.key === Qt.Key_S) { pr.submit(); e.accepted = true }
  }
}

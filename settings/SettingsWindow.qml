import QtQuick
import Quickshell
import qs.common
import "../components"
import "../components/ui"
import "../common/Keys.js" as KeyNames

// CLIP//NET settings: a normal window (Ctrl+, in the popup, or
// `clipnet settings`). Every change is saved to the daemon immediately.
FloatingWindow {
  id: win

  property int page: 0
  readonly property var pages: ["General", "Capture", "History", "Paste", "Buffers", "Rules", "Hotkeys", "Data"]
  property var hotkeyList: []
  property var groupList: []
  property var ruleList: []
  property var bufferList: []

  function loadHotkeys() { Daemon.call("hotkeys.list", {}, r => { if (r) hotkeyList = r }) }
  function loadGroups() { Daemon.call("groups.list", {}, r => { if (r) groupList = r }) }
  function loadRules() { Daemon.call("rules.list", {}, r => { if (r) ruleList = r }) }
  function loadBuffers() { Daemon.call("buffers.get", {}, r => { if (r) bufferList = r }) }
  function groupName(uuid) {
    const g = groupList.find(x => x.uuid === uuid)
    return g ? g.name : "(deleted group)"
  }

  Connections {
    target: Daemon
    function onEvent(name, data) {
      if (!win.visible) return
      if (name === "hotkeys.changed") win.loadHotkeys()
      else if (name === "groups.changed") win.loadGroups()
      else if (name === "rules.changed") win.loadRules()
      else if (name === "buffers.changed" || name.startsWith("clip")) win.loadBuffers()
    }
  }

  title: "CLIP//NET settings"
  color: Theme.card
  implicitWidth: Theme.px(760)
  implicitHeight: Theme.px(600)
  minimumSize: Qt.size(Theme.px(620), Theme.px(460))

  function openPage(p) {
    if (p !== undefined) page = p
    visible = true
    nav.forceActiveFocus()
  }

  onVisibleChanged: if (visible) { stats.load(); hotkeys.load(); loadHotkeys(); loadGroups(); loadRules(); loadBuffers() }

  // ---- navigation -----------------------------------------------------------

  Rectangle {
    id: side
    width: Theme.px(170)
    height: parent.height
    color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.035)

    Column {
      id: nav
      anchors.fill: parent
      anchors.margins: Theme.px(10)
      spacing: Theme.px(2)
      focus: true
      Text {
        text: "CLIP//NET"
        color: Theme.accent
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSize + 3
        font.bold: true
        bottomPadding: Theme.px(10)
      }
      Repeater {
        model: win.pages
        Rectangle {
          required property int index
          required property string modelData
          width: nav.width
          height: Math.round(Theme.fontSize * 2.2)
          radius: Math.min(Theme.radius, 6)
          color: index === win.page ? Theme.selectedBackground : ma.containsMouse ? Theme.faint : "transparent"
          Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.px(10)
            anchors.verticalCenter: parent.verticalCenter
            text: modelData
            color: index === win.page ? Theme.selectedText : Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
          }
          MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; onClicked: win.page = index }
        }
      }
      Keys.onPressed: e => {
        if (e.key === Qt.Key_Down) win.page = Math.min(win.pages.length - 1, win.page + 1)
        else if (e.key === Qt.Key_Up) win.page = Math.max(0, win.page - 1)
        else if (e.key === Qt.Key_Escape || ((e.modifiers & Qt.ControlModifier) && e.key === Qt.Key_W)) win.visible = false
        else return
        e.accepted = true
      }
    }
  }

  Flickable {
    id: flick
    anchors.left: side.right
    anchors.right: parent.right
    anchors.top: parent.top
    anchors.bottom: parent.bottom
    anchors.margins: Theme.px(18)
    contentHeight: content.height + Theme.px(20)
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Item {
      id: content
      width: flick.width
      height: [general, capture, history, paste, buffersPage, rulesPage, hotkeysPage, data][win.page].height

      // ---- General ----------------------------------------------------------
      Column {
        id: general
        visible: win.page === 0
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "Popup"
          SettingChoice {
            key: "popup_position"
            label: "Open at"
            options: [{ value: "cursor", label: "Mouse cursor" }, { value: "center", label: "Screen centre" }, { value: "last", label: "Last position" }]
          }
          SettingNumber { key: "popup_width"; label: "Width"; unit: "px (before font scaling)" }
          SettingNumber { key: "popup_rows"; label: "Rows shown"; unit: "rows" }
          SettingNumber { key: "scrim_alpha"; label: "Dim the screen behind it"; unit: "% (0 = off, like Ditto)" }
        }
        Section {
          title: "Behaviour"
          SettingToggle { key: "show_thumbnails"; text: "Show image thumbnails in the list" }
          SettingToggle { key: "show_grouped_in_history"; text: "Show clips that are in groups in History too" }
          SettingToggle {
            key: "esc_clears_search"
            text: "Esc clears the search before closing"
            hint: "Ditto closes on the first Esc; Shift+Esc always closes."
          }
        }
      }

      // ---- Capture ----------------------------------------------------------
      Column {
        id: capture
        visible: win.page === 1
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "Recording"
          Toggle {
            width: parent.width
            text: "Pause recording"
            hint: Daemon.paused ? "Copies are not being saved right now." : "Stop saving copies until you turn this off."
            checked: Daemon.paused
            onToggled: c => Daemon.call(c ? "pause" : "resume", {})
          }
          Row {
            spacing: Theme.px(8)
            Repeater {
              model: [15, 60, 240]
              Btn {
                required property int modelData
                text: "Pause " + (modelData < 60 ? modelData + " min" : modelData / 60 + " h")
                onClicked: Daemon.call("pause", { minutes: modelData })
              }
            }
          }
          SettingToggle {
            key: "keep_alive"
            text: "Keep the clipboard when the app you copied from closes"
            hint: "Never after a password manager clears it on purpose."
          }
          SettingToggle { key: "store_source_title"; text: "Remember the window title a clip was copied from" }
          SettingToggle {
            key: "capture_primary"
            text: "Also record selected text (select-to-copy)"
            hint: "Linux's second clipboard: whatever you highlight. Off by default, since every selection would land in the history. Saved once the selection has held still for a moment."
          }
          HotkeyBinder { label: "Hotkey to pause / resume recording"; action: "pause_toggle"; hotkeys: win.hotkeyList; onChanged: win.loadHotkeys() }
        }
        Section {
          title: "Size limits per format"
          SettingNumber { key: "text_cap_bytes"; label: "Text"; unit: "MB"; scale: 1048576 }
          SettingNumber { key: "image_cap_bytes"; label: "Images"; unit: "MB"; scale: 1048576 }
          SettingNumber { key: "other_cap_bytes"; label: "Other formats"; unit: "MB"; scale: 1048576; hint: "A format over its limit is skipped; the rest of the copy is kept." }
        }
      }

      // ---- History (retention) ----------------------------------------------
      Column {
        id: history
        visible: win.page === 2
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "How much to keep"
          Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Sticky, grouped and locked clips, and clips with a hotkey or quick-paste word, are never removed automatically."
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
          SettingNumber { key: "max_clips"; label: "Keep at most"; unit: "clips (0 = unlimited)" }
          SettingNumber { key: "max_age_days"; label: "Remove clips unused for"; unit: "days (0 = never)" }
          SettingNumber { key: "large_bytes"; label: "Large clips are those over"; unit: "MB"; scale: 1048576 }
          SettingNumber { key: "large_age_days"; label: "Remove large clips unused for"; unit: "days (0 = never)" }
          Row {
            spacing: Theme.px(10)
            Btn {
              text: "Apply now"
              onClicked: Daemon.call("retention.run", {}, (r, err) => retentionResult.text = err ? err.message : r.removed + " clips removed")
            }
            Text {
              id: retentionResult
              anchors.verticalCenter: parent.verticalCenter
              color: Theme.dim
              font.family: Theme.fontFamily
              font.pixelSize: Theme.fontSize - 1
            }
          }
        }
      }

      // ---- Paste ------------------------------------------------------------
      Column {
        id: paste
        visible: win.page === 3
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "Pasting"
          SettingToggle { key: "move_to_top_on_paste"; text: "Move a pasted clip to the top of the history" }
          SettingNumber { key: "paste_delay_ms"; label: "Wait before pressing paste"; unit: "ms"; hint: "Raise this if an app misses pastes." }
          Field {
            id: sepField
            width: Theme.px(200)
            label: "Between clips pasted together (\\n = new line, \\t = tab)"
            Component.onCompleted: text = JSON.stringify(Daemon.setting("multi_separator", "\n")).slice(1, -1)
            onAccepted: {
              let v
              try { v = JSON.parse("\"" + text.replace(/"/g, "\\\"") + "\"") } catch (e) { v = text }
              Daemon.call("settings.set", { key: "multi_separator", value: v })
            }
          }
          Field {
            width: Theme.px(260)
            label: "Date format for Special Paste (strftime, Enter saves)"
            Component.onCompleted: text = Daemon.setting("date_format", "%Y-%m-%d %H:%M")
            onAccepted: Daemon.call("settings.set", { key: "date_format", value: text })
          }
        }
      }

      // ---- Copy buffers -----------------------------------------------------
      Column {
        id: buffersPage
        visible: win.page === 4
        width: parent.width
        spacing: Theme.px(22)
        Text {
          width: parent.width
          wrapMode: Text.Wrap
          text: "Three extra clipboards, as in Ditto. \u201cCopy into buffer\u201d copies the selection in the focused window into the buffer and leaves your normal clipboard as it was; \u201cpaste buffer\u201d pastes it and puts the normal clipboard back. Give them hotkeys here."
          color: Theme.dim
          font.family: Theme.fontFamily
          font.pixelSize: Theme.fontSize - 1
        }
        Repeater {
          model: 3
          Section {
            required property int index
            readonly property int slot: index + 1
            readonly property var held: (win.bufferList.find(b => b.slot === slot) || {}).clip || null
            title: "Buffer " + slot
            Text {
              width: parent.width
              elide: Text.ElideRight
              text: held ? "Holds: " + (held.title || held.preview) : "Empty"
              color: held ? Theme.text : Theme.dim
              font.family: Theme.fontFamily
              font.pixelSize: Theme.fontSize
            }
            Row {
              width: parent.width
              spacing: Theme.px(10)
              HotkeyBinder { width: (parent.width - Theme.px(20)) / 3; label: "Copy into it"; action: "buffer_copy"; arg: String(slot); hotkeys: win.hotkeyList; onChanged: win.loadHotkeys() }
              HotkeyBinder { width: (parent.width - Theme.px(20)) / 3; label: "Cut into it"; action: "buffer_cut"; arg: String(slot); hotkeys: win.hotkeyList; onChanged: win.loadHotkeys() }
              HotkeyBinder { width: (parent.width - Theme.px(20)) / 3; label: "Paste it"; action: "buffer_paste"; arg: String(slot); hotkeys: win.hotkeyList; onChanged: win.loadHotkeys() }
            }
          }
        }
      }

      // ---- Rules --------------------------------------------------------------
      Column {
        id: rulesPage
        visible: win.page === 5
        width: parent.width
        spacing: Theme.px(22)

        function describe(r) {
          const app = r.match_app ? "\u201c" + r.match_app + "\u201d" : "any app"
          switch (r.action) {
          case "exclude": return "Ignore copies from " + app
          case "skip_mime": return "Drop " + r.match_mime + " from copies" + (r.match_app ? " made in " + app : "")
          case "paste_keys": return "Paste with " + KeyNames.pretty(r.arg ? r.arg.keys : "?") + " in " + app
          case "to_group": return "File copies from " + app + " in \u201c" + win.groupName(r.arg ? r.arg.group : "") + "\u201d"
          }
          return r.action
        }

        Section {
          title: "Rules"
          Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Apps are matched by their window class, with * as a wildcard and case ignored (see classes with: hyprctl clients). Rules apply top to bottom."
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
          Repeater {
            model: win.ruleList
            Row {
              required property var modelData
              required property int index
              width: parent.width
              spacing: Theme.px(8)
              Toggle {
                width: parent.width - Theme.px(200)
                text: rulesPage.describe(modelData)
                checked: modelData.enabled
                onToggled: c => {
                  const spec = { id: modelData.id, enabled: c, action: modelData.action,
                                 match_app: modelData.match_app, match_mime: modelData.match_mime, arg: modelData.arg }
                  Daemon.call("rules.set", spec, (r, err) => { if (err) console.warn("clipnet:", err.message) })
                }
              }
              Btn {
                text: "\u2191"
                implicitWidth: Theme.px(34)
                enabled: index > 0
                opacity: enabled ? 1 : 0.3
                onClicked: {
                  const ids = win.ruleList.map(x => x.id)
                  ids.splice(index - 1, 2, ids[index], ids[index - 1])
                  Daemon.call("rules.reorder", { ids: ids })
                }
              }
              Btn {
                text: "\u2193"
                implicitWidth: Theme.px(34)
                enabled: index < win.ruleList.length - 1
                opacity: enabled ? 1 : 0.3
                onClicked: {
                  const ids = win.ruleList.map(x => x.id)
                  ids.splice(index, 2, ids[index + 1], ids[index])
                  Daemon.call("rules.reorder", { ids: ids })
                }
              }
              Btn { text: "Delete"; danger: true; onClicked: Daemon.call("rules.delete", { id: modelData.id }) }
            }
          }
        }

        Section {
          id: newRule
          title: "Add a rule"
          property string action: "exclude"
          property string groupUuid: ""
          property string error: ""
          Flow {
            width: parent.width
            spacing: Theme.px(4)
            Repeater {
              model: [{ v: "exclude", l: "Ignore an app" }, { v: "skip_mime", l: "Drop a format" },
                      { v: "paste_keys", l: "Paste key for an app" }, { v: "to_group", l: "File an app's copies" }]
              Btn {
                required property var modelData
                text: modelData.l
                primary: newRule.action === modelData.v
                onClicked: newRule.action = modelData.v
              }
            }
          }
          Field { id: ruleApp; width: parent.width; label: newRule.action === "skip_mime" ? "App (optional)" : "App"; placeholder: "e.g. *slack* or com.mitchellh.ghostty" }
          Field { id: ruleMime; visible: newRule.action === "skip_mime"; width: parent.width; label: "Format"; placeholder: "e.g. image/* or text/html" }
          HotkeyField { id: ruleKeys; visible: newRule.action === "paste_keys"; width: Theme.px(260); label: "Keys that paste in that app" }
          Btn {
            visible: newRule.action === "to_group"
            text: newRule.groupUuid ? "Group: " + win.groupName(newRule.groupUuid) : "Choose a group\u2026"
            onClicked: {
              if (!win.groupList.length) { newRule.error = "Make a group first (F7 in the popup)."; return }
              const items = win.groupList.map(g => ({ label: g.name, action: "g", data: g.uuid }))
              const m = menuComp.createObject(content, { items: items, px: Theme.px(40), py: Theme.px(40) })
              m.triggered.connect((a, d) => { newRule.groupUuid = d; m.destroy() })
              m.closed.connect(() => m.destroy())
              m.focusMenu()
            }
          }
          Btn {
            text: "Add rule"
            primary: true
            onClicked: {
              const spec = { action: newRule.action, match_app: ruleApp.text, match_mime: ruleMime.text }
              if (newRule.action === "paste_keys") spec.arg = { keys: ruleKeys.accel }
              if (newRule.action === "to_group") spec.arg = { group: newRule.groupUuid }
              Daemon.call("rules.set", spec, (r, err) => {
                newRule.error = err ? err.message : ""
                if (!err) { ruleApp.text = ""; ruleMime.text = ""; ruleKeys.accel = "" }
              })
            }
          }
          Text {
            visible: !!newRule.error
            width: parent.width
            wrapMode: Text.Wrap
            text: newRule.error
            color: Theme.urgent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
        }
        Component { id: menuComp; Menu {} }
      }

      // ---- Hotkeys ----------------------------------------------------------
      Column {
        id: hotkeysPage
        visible: win.page === 6
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "Hotkeys"
          Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Ctrl+' opens the history (set in Hyprland's bindings). Give a clip its own hotkey from its Properties (Alt+Enter in the popup). Here you can also bind history positions: Ditto's “paste the Nth most recent clip”."
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
          Repeater {
            id: hotkeys
            property var items: []
            function load() { Daemon.call("hotkeys.list", {}, (r) => { if (r) items = r }) }
            model: items
            Row {
              required property var modelData
              spacing: Theme.px(12)
              Text {
                width: Theme.px(160)
                text: KeyNames.pretty(modelData.accel)
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize
              }
              Text {
                width: Theme.px(260)
                elide: Text.ElideRight
                text: modelData.label + (modelData.conflict ? "  ⚠ taken by “" + modelData.conflict + "”" : "")
                color: modelData.conflict ? Theme.urgent : Theme.dim
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSize
              }
              Btn { text: "Remove"; onClicked: Daemon.call("hotkeys.remove", { id: modelData.id }, () => hotkeys.load()) }
            }
          }
          Text {
            visible: !hotkeys.items.length
            text: "No hotkeys yet."
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
          }
        }
        Section {
          title: "Add a history-position hotkey"
          Row {
            spacing: Theme.px(10)
            HotkeyField { id: posKeys; width: Theme.px(240); label: "Keys" }
            Field { id: posN; width: Theme.px(90); label: "Position"; numeric: true; text: "1" }
            Btn {
              anchors.bottom: parent.bottom
              text: "Add"
              onClicked: Daemon.call("hotkeys.set", { accel: posKeys.accel, action: "paste_position", arg: posN.text || "1" }, (r, err) => {
                posError.text = err ? err.message : ""
                if (!err) { posKeys.accel = ""; hotkeys.load() }
              })
            }
          }
          Text {
            id: posError
            visible: !!text
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.urgent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
        }
      }

      // ---- Data -------------------------------------------------------------
      Column {
        id: data
        visible: win.page === 7
        width: parent.width
        spacing: Theme.px(22)
        Section {
          title: "Your history"
          Text {
            id: stats
            property var s: null
            function load() { Daemon.call("stats", {}, (r) => { if (r) s = r }) }
            width: parent.width
            wrapMode: Text.Wrap
            text: !s ? "…" : s.count + " clips · " + Format.size(s.content_bytes) + " of content · database " +
                  Format.size(s.db_bytes) + " · stored in ~/.local/share/clipnet"
            color: Theme.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize
          }
        }
        Section {
          title: "Export and import"
          Field {
            id: exportPath
            width: parent.width
            label: "File"
            Component.onCompleted: {
              const d = new Date()
              text = Quickshell.env("HOME") + "/clipnet-export-" + d.getFullYear() + Format.pad(d.getMonth() + 1) + Format.pad(d.getDate()) + ".json"
            }
          }
          Row {
            spacing: Theme.px(8)
            Btn {
              text: "Export everything"
              onClicked: Daemon.call("export", { path: exportPath.text }, (r, err) =>
                dataResult.text = err ? err.message : "Exported " + r.clips + " clips and " + r.groups + " groups.")
            }
            Btn {
              text: "Import this file"
              onClicked: Daemon.call("import", { format: "clipnet-json", path: exportPath.text }, (r, err) => {
                dataResult.text = err ? err.message : "Imported " + r.added + " clips (" + r.duplicates + " already here)."
                stats.load()
              })
            }
            Btn {
              text: "Import Omarchy's history"
              onClicked: Daemon.call("import", { format: "omarchy" }, (r, err) => {
                dataResult.text = err ? err.message : "Imported " + r.added + " clips (" + r.duplicates + " already here)."
                stats.load()
              })
            }
          }
          Text {
            id: dataResult
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 1
          }
          Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Exports hold every format of every clip (images included) and are private to you (0600). Importing skips clips already here."
            color: Theme.dim
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSize - 2
          }
        }
      }
    }
  }
}

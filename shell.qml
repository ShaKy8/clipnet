import QtQuick
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import qs.common
import "components"

// CLIP//NET — Ditto's clipboard history for Omarchy.
//
// This is only the UI. clipnetd (daemon/) watches the clipboard, stores the
// history and does the pasting; the UI asks it for rows and tells it what to
// paste. Either can restart without the other.
//
// Ctrl+' (and Super+Ctrl+V) reach `toggle` through hl.dsp.global, which
// Hyprland delivers straight to this process: no fork per keypress.
//
// IPC: quickshell ipc -p <this dir> call clipnet toggle | show | hide
ShellRoot {
  id: shell

  Popup { id: popup }

  GlobalShortcut {
    appid: "clipnet"; name: "toggle"
    description: "Show or hide the clipboard history"
    onPressed: popup.toggle()
  }
  GlobalShortcut {
    appid: "clipnet"; name: "show"
    description: "Show the clipboard history"
    onPressed: if (!popup.open) popup.show()
  }

  IpcHandler {
    target: "clipnet"
    function toggle(): void { popup.toggle() }
    function show(): void { if (!popup.open) popup.show() }
    function hide(): void { popup.hide() }
    function reloadTheme(): void { Theme.reload() }
  }
}

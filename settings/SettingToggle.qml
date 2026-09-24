import QtQuick
import qs.common
import "../components/ui"

// A boolean daemon setting.
Toggle {
  property string key
  width: parent ? parent.width : 0
  checked: Daemon.setting(key, false) === true
  onToggled: c => Daemon.call("settings.set", { key: key, value: c }, (r, err) => {
    if (err) { checked = !c; console.warn("clipnet:", err.message) }
  })
}

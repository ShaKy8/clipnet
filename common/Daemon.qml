pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

// The UI's line to clipnetd: JSON lines over its Unix socket.
//
//   Daemon.call("list", { query: "foo" }, (result, error) => ...)
//   Connections { target: Daemon; function onEvent(name, data) { ... } }
//
// Reconnects by itself (the daemon may start after us or restart), and
// answers calls made while offline with an "offline" error at once rather
// than queueing them: a stale paste replayed later would be worse than none.
Singleton {
  id: root

  readonly property string path: Quickshell.env("XDG_RUNTIME_DIR") + "/clipnet/clipnetd.sock"
  readonly property bool online: sock.connected && ready
  property bool ready: false
  property bool paused: false
  property real pausedUntil: 0   // ms; 0 = until resumed
  property bool hyprland: false
  property string version: ""
  // The daemon's settings, kept current through settings.changed events.
  property var settings: ({})
  function setting(key, fallback) {
    const v = settings[key]
    return v === undefined ? fallback : v
  }

  signal event(string name, var data)
  signal connectedAgain()

  property int nextId: 1
  property var pending: ({})

  function call(op, args, cb) {
    if (!sock.connected) {
      if (cb) cb(null, { code: "offline", message: "clipnetd is not running" })
      return
    }
    const id = nextId++
    // "rid", not "id": operations use "id" for the clip or group.
    const req = Object.assign({}, args || {}, { rid: id, op: op })
    if (cb) pending[id] = cb
    sock.write(JSON.stringify(req) + "\n")
    sock.flush()
  }

  function handle(line) {
    let msg
    try { msg = JSON.parse(line) } catch (e) { console.warn("clipnet: bad line from daemon"); return }
    if (msg.event !== undefined) {
      if (msg.event === "state.changed") { paused = !!msg.data.paused; pausedUntil = msg.data.paused_until || 0 }
      if (msg.event === "settings.changed") {
        const next = Object.assign({}, settings)
        next[msg.data.key] = msg.data.value
        settings = next
      }
      root.event(msg.event, msg.data)
      return
    }
    const cb = pending[msg.rid]
    delete pending[msg.rid]
    if (cb) cb(msg.ok ? msg.result : null, msg.ok ? null : msg.error)
  }

  function failPending() {
    const p = pending
    pending = {}
    for (const id in p) p[id](null, { code: "offline", message: "connection to clipnetd lost" })
  }

  Socket {
    id: sock
    path: root.path
    connected: true
    parser: SplitParser { onRead: data => root.handle(data) }
    onConnectedChanged: {
      if (connected) {
        retry.interval = 250
        root.call("hello", { client: "clipnet-ui", subscribe: true }, (r, err) => {
          if (!r) return
          root.version = r.version
          root.paused = r.paused
          root.call("state.get", {}, st => { if (st) root.pausedUntil = st.paused_until || 0 })
          root.hyprland = r.hyprland
          root.ready = true
          root.call("settings.get", {}, s => { if (s) root.settings = s })
          root.connectedAgain()
        })
      } else {
        root.ready = false
        root.failPending()
        retry.restart()
      }
    }
    onError: retry.restart()
  }

  // Back off to one attempt every 2 s while the daemon is down.
  Timer {
    id: retry
    interval: 250
    onTriggered: {
      if (sock.connected) return
      sock.connected = true
      interval = Math.min(interval * 2, 2000)
    }
  }
}

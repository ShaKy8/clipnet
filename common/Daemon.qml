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

  // CLIPNET_SOCKET points the UI at another daemon (development, demos).
  readonly property string path: Quickshell.env("CLIPNET_SOCKET") || (Quickshell.env("XDG_RUNTIME_DIR") + "/clipnet/clipnetd.sock")
  readonly property bool online: !!sock && sock.connected && ready
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
    if (!sock || !sock.connected) {
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

  // One Socket per connection attempt. A Quickshell Socket that failed to
  // connect does not reliably try again when `connected` is set, which left
  // the popup offline for good after the daemon restarted; a fresh object
  // always makes a fresh attempt.
  property Socket sock: null

  Component {
    id: socketComp
    Socket {
      id: s
      path: root.path
      connected: false // connected by reconnect(), once root.sock points here
      parser: SplitParser { onRead: data => root.handle(data) }
      onConnectedChanged: {
        if (s !== root.sock) return
        if (connected) {
          root.handshakeSince = Date.now()
          root.call("hello", { client: "clipnet-ui", subscribe: true }, (r, err) => {
            if (!r) return
            root.version = r.version
            root.paused = r.paused
            root.hyprland = r.hyprland
            root.ready = true
            root.call("state.get", {}, st => { if (st) root.pausedUntil = st.paused_until || 0 })
            root.call("settings.get", {}, s => { if (s) root.settings = s })
            root.connectedAgain()
          })
        } else {
          root.ready = false
          root.failPending()
        }
      }
    }
  }

  property real handshakeSince: 0

  function reconnect() {
    // Connected and waiting for hello's reply: give it a moment.
    if (sock && sock.connected && Date.now() - handshakeSince < 2000) return
    if (sock) {
      const old = sock
      sock = null
      old.destroy()
    }
    // Assign before connecting: a local socket may connect at once, and the
    // handshake must find root.sock already set.
    sock = socketComp.createObject(root)
    if (sock) sock.connected = true
  }

  Component.onCompleted: reconnect()

  // Until the handshake succeeds, try again twice a second.
  Timer {
    interval: 500
    repeat: true
    running: !root.ready
    onTriggered: root.reconnect()
  }
}

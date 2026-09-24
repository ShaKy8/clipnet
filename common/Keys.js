.pragma library

// Qt key events → the daemon's portable shortcut names ("Ctrl+Alt+1").
//
// With Shift held, Qt reports the shifted symbol (Shift+' arrives as
// Key_QuoteDbl), so shifted symbols map back to their unshifted key. This
// assumes a US layout, which is also what Hyprland's keysym binds assume.

var named = {}

function init(Qt) {
  if (named.done) return
  var n = named
  for (var c = 0; c < 26; c++) n[Qt.Key_A + c] = String.fromCharCode(65 + c)
  for (var d = 0; d < 10; d++) n[Qt.Key_0 + d] = String(d)
  for (var f = 1; f <= 24; f++) n[Qt.Key_F1 + f - 1] = "F" + f
  var pairs = [
    [Qt.Key_Apostrophe, "Apostrophe"], [Qt.Key_QuoteDbl, "Apostrophe"],
    [Qt.Key_QuoteLeft, "Grave"], [Qt.Key_AsciiTilde, "Grave"],
    [Qt.Key_Comma, "Comma"], [Qt.Key_Less, "Comma"],
    [Qt.Key_Period, "Period"], [Qt.Key_Greater, "Period"],
    [Qt.Key_Slash, "Slash"], [Qt.Key_Question, "Slash"],
    [Qt.Key_Backslash, "Backslash"], [Qt.Key_Bar, "Backslash"],
    [Qt.Key_Semicolon, "Semicolon"], [Qt.Key_Colon, "Semicolon"],
    [Qt.Key_Minus, "Minus"], [Qt.Key_Underscore, "Minus"],
    [Qt.Key_Equal, "Equal"], [Qt.Key_Plus, "Equal"],
    [Qt.Key_BracketLeft, "BracketLeft"], [Qt.Key_BraceLeft, "BracketLeft"],
    [Qt.Key_BracketRight, "BracketRight"], [Qt.Key_BraceRight, "BracketRight"],
    [Qt.Key_Exclam, "1"], [Qt.Key_At, "2"], [Qt.Key_NumberSign, "3"], [Qt.Key_Dollar, "4"],
    [Qt.Key_Percent, "5"], [Qt.Key_AsciiCircum, "6"], [Qt.Key_Ampersand, "7"], [Qt.Key_Asterisk, "8"],
    [Qt.Key_ParenLeft, "9"], [Qt.Key_ParenRight, "0"],
    [Qt.Key_Space, "Space"], [Qt.Key_Tab, "Tab"], [Qt.Key_Backtab, "Tab"],
    [Qt.Key_Return, "Return"], [Qt.Key_Enter, "Return"], [Qt.Key_Insert, "Insert"],
    [Qt.Key_Delete, "Delete"], [Qt.Key_Home, "Home"], [Qt.Key_End, "End"],
    [Qt.Key_PageUp, "PageUp"], [Qt.Key_PageDown, "PageDown"],
    [Qt.Key_Left, "Left"], [Qt.Key_Right, "Right"], [Qt.Key_Up, "Up"], [Qt.Key_Down, "Down"],
    [Qt.Key_Print, "Print"],
  ]
  for (var i = 0; i < pairs.length; i++) n[pairs[i][0]] = pairs[i][1]
  n.done = true
}

// "Ctrl+Alt+1", "" while only modifiers are held, or null for keys a
// shortcut cannot use. Canonical modifier order matches the daemon's.
function accel(Qt, event) {
  init(Qt)
  var key = named[event.key]
  if (key === undefined) return null
  var m = event.modifiers
  return (m & Qt.MetaModifier ? "Super+" : "") + (m & Qt.ControlModifier ? "Ctrl+" : "") +
         (m & Qt.AltModifier ? "Alt+" : "") + (m & Qt.ShiftModifier ? "Shift+" : "") + key
}

function isModifier(Qt, key) {
  return key === Qt.Key_Control || key === Qt.Key_Shift || key === Qt.Key_Alt || key === Qt.Key_Meta ||
         key === Qt.Key_Super_L || key === Qt.Key_Super_R || key === Qt.Key_AltGr
}

// For display: "Ctrl+Alt+1" → "Ctrl+Alt+1", "Ctrl+Apostrophe" → "Ctrl+'".
function pretty(accel) {
  var sym = { Apostrophe: "'", Grave: "`", Comma: ",", Period: ".", Slash: "/", Backslash: "\\",
              Semicolon: ";", Minus: "-", Equal: "=", BracketLeft: "[", BracketRight: "]" }
  var parts = String(accel || "").split("+")
  var k = parts[parts.length - 1]
  if (sym[k]) parts[parts.length - 1] = sym[k]
  return parts.join("+")
}

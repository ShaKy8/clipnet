.pragma library

// Small display helpers shared by the popup and settings.

// "now", "42s", "5m", "3h", "2d", "3w", then a date. Compact on purpose: it
// sits in a narrow right-hand column like Ditto's.
function age(ms, now) {
  var s = Math.max(0, Math.floor(((now || Date.now()) - ms) / 1000))
  if (s < 5) return "now"
  if (s < 60) return s + "s"
  if (s < 3600) return Math.floor(s / 60) + "m"
  if (s < 86400) return Math.floor(s / 3600) + "h"
  if (s < 86400 * 14) return Math.floor(s / 86400) + "d"
  if (s < 86400 * 60) return Math.floor(s / (86400 * 7)) + "w"
  var d = new Date(ms)
  return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate())
}

function pad(n) {
  return n < 10 ? "0" + n : "" + n
}

function size(bytes) {
  if (bytes < 1024) return bytes + " B"
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB"
  if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB"
  return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB"
}

// One glyph per kind, from the Nerd Font Omarchy ships (JetBrainsMono NF);
// the plain fallbacks read fine in any font.
function glyph(kind, nerd) {
  if (!nerd) return { text: "T", rich: "R", image: "I", files: "F", other: "?" }[kind] || "?"
  return { text: "", rich: "", image: "", files: "", other: "" }[kind] || ""
}

// A date and time for the details line.
function stamp(ms) {
  var d = new Date(ms)
  return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes())
}

// Ditto's quick-paste numbers: rows 1–9 are Ctrl+1..9, the tenth is Ctrl+0.
function positionLabel(index) {
  if (index < 9) return "" + (index + 1)
  if (index === 9) return "0"
  return ""
}

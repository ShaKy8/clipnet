.pragma library

// Omarchy theme files → the colors CLIP//NET draws with.
//
// Mirrors Omarchy's own resolution (shell/Commons/Color.qml) closely enough
// that the popup matches the Omarchy menus: the foundational palette from
// colors.toml, per-surface [menu] tokens from shell.toml, role references
// like "hyprland.active-border-foreground", gradients reduced to their first
// stop, and `-alpha` companions. Kept free of QML types so it can be tested.

function trim(s) {
  return String(s === undefined || s === null ? "" : s).replace(/^\s+|\s+$/g, "")
}

// colors.toml: flat `key = "#rrggbb"` lines.
function parseColors(raw) {
  var out = {}
  var lines = String(raw || "").split("\n")
  for (var i = 0; i < lines.length; i++) {
    var m = lines[i].match(/^\s*([A-Za-z0-9_-]+)\s*=\s*["']?(#[0-9A-Fa-f]{6})/)
    if (m) out[m[1]] = m[2]
  }
  return out
}

// shell.toml: "section.key" → raw string. Accepts quoted strings, numbers,
// width lists and bare role names; tolerates inline comments.
function parseShell(raw) {
  var parsed = {}
  var lines = String(raw || "").split("\n")
  var section = ""
  for (var i = 0; i < lines.length; i++) {
    var line = trim(lines[i])
    if (!line || line.charAt(0) === "#") continue
    var sec = line.match(/^\[([A-Za-z0-9_-]+)\]\s*(#.*)?$/)
    if (sec) { section = sec[1]; continue }
    var kv = line.match(/^([A-Za-z0-9_-]+)\s*=\s*["']([^"']+)["']\s*(#.*)?$/)
      || line.match(/^([A-Za-z0-9_-]+)\s*=\s*(-?\d+(?:\.\d+)?(?:\s+-?\d+(?:\.\d+)?){0,3})\s*(#.*)?$/)
      || line.match(/^([A-Za-z0-9_-]+)\s*=\s*([A-Za-z][A-Za-z0-9_-]*)\s*(#.*)?$/)
    if (kv && section) parsed[section + "." + kv[1]] = kv[2]
  }
  return parsed
}

function merge(base, over) {
  var m = {}
  for (var a in base) m[a] = base[a]
  for (var b in over) m[b] = over[b]
  return m
}

// The foundational palette, with Omarchy's fallbacks for older themes that
// only define color0..color15.
function palette(colors) {
  var c = colors || {}
  var fg = c.foreground || c.color7 || "#cacccc"
  return {
    foreground: fg,
    background: c.background || c.color0 || "#101315",
    accent: c.accent || c.color4 || "#cacccc",
    urgent: c.red || c.color1 || "#a55555",
    muted: c.muted || c.color8 || fg,
    selection: c.selection || c.accent || c.color4 || fg,
  }
}

function hex2(n) {
  var s = Math.max(0, Math.min(255, Math.round(n))).toString(16)
  return s.length < 2 ? "0" + s : s
}

// "#rgb", "#rrggbb", "#rrggbbaa", "rgb(rrggbb)", "rgba(rrggbbaa)",
// "rgb(r,g,b)" → {hex: "#rrggbb", a: 0..1}, or null.
function parseColor(token) {
  var s = trim(token)
  var m
  if ((m = s.match(/^#([0-9A-Fa-f])([0-9A-Fa-f])([0-9A-Fa-f])$/)))
    return { hex: "#" + m[1] + m[1] + m[2] + m[2] + m[3] + m[3], a: 1 }
  if ((m = s.match(/^#([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?$/)))
    return { hex: "#" + m[1].toLowerCase(), a: m[2] ? parseInt(m[2], 16) / 255 : 1 }
  if ((m = s.match(/^rgb\(([0-9A-Fa-f]{6})\)$/i))) return { hex: "#" + m[1].toLowerCase(), a: 1 }
  if ((m = s.match(/^rgba\(([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})\)$/i)))
    return { hex: "#" + m[1].toLowerCase(), a: parseInt(m[2], 16) / 255 }
  if ((m = s.match(/^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$/i)))
    return { hex: "#" + hex2(+m[1]) + hex2(+m[2]) + hex2(+m[3]), a: 1 }
  return null
}

// A gradient ("rgba(..) rgba(..) 45deg") contributes its first stop.
function firstToken(value) {
  var parts = trim(value).split(/\s+/)
  for (var i = 0; i < parts.length; i++)
    if (!parts[i].match(/^-?\d+(?:\.\d+)?deg$/)) return parts[i]
  return trim(value)
}

// Resolve a token to {hex, a}: a color literal, a palette role, or a
// reference to another shell value (followed at most 8 deep).
function resolve(value, shell, pal, fallbackHex, depth) {
  var token = firstToken(value)
  var role = token.toLowerCase()
  if ((depth || 0) < 8 && shell[role] !== undefined && shell[role] !== token)
    return resolve(shell[role], shell, pal, fallbackHex, (depth || 0) + 1)
  if (role === "foreground" || role === "text") return { hex: pal.foreground, a: 1 }
  if (role === "background") return { hex: pal.background, a: 1 }
  if (role === "accent") return { hex: pal.accent, a: 1 }
  if (role === "urgent") return { hex: pal.urgent, a: 1 }
  if (role === "muted") return { hex: pal.muted, a: 1 }
  if (role === "transparent") return { hex: "#000000", a: 0 }
  return parseColor(token) || parseColor(fallbackHex) || { hex: "#ff00ff", a: 1 }
}

function alphaOf(shell, key, fallback) {
  var v = shell[key]
  if (v === undefined) return fallback
  var n = Number(v)
  return isFinite(n) ? Math.max(0, Math.min(1, n)) : fallback
}

// "#rrggbb" + alpha → "#aarrggbb" (the form QML's color type accepts).
function argb(c, alpha) {
  var a = (c.a === undefined ? 1 : c.a) * (alpha === undefined ? 1 : alpha)
  return "#" + hex2(a * 255) + c.hex.substring(1)
}

// Everything the popup needs, as QML-ready color strings plus sizes.
function compute(colorsRaw, themeShellRaw, userShellRaw) {
  var pal = palette(parseColors(colorsRaw))
  var shell = merge(parseShell(themeShellRaw), parseShell(userShellRaw))
  function surface(key, fallbackHex, fallbackAlpha) {
    var v = shell["menu." + key]
    var c = v !== undefined ? resolve(v, shell, pal, fallbackHex) : parseColor(fallbackHex)
    return argb(c, alphaOf(shell, "menu." + key + "-alpha", fallbackAlpha))
  }
  var base = Math.max(1, Number(shell["font.base-size"]) || 12)
  return {
    foreground: argb(parseColor(pal.foreground)),
    background: argb(parseColor(pal.background)),
    accent: argb(parseColor(pal.accent)),
    urgent: argb(parseColor(pal.urgent)),
    muted: argb(parseColor(pal.muted)),
    selection: argb(parseColor(pal.selection)),
    card: surface("background", pal.background, 1.0),
    text: surface("text", pal.foreground, 1.0),
    border: surface("border", pal.foreground, 1.0),
    scrim: surface("scrim", pal.background, 0.5),
    selectedBackground: surface("selected-background", pal.foreground, 0.08),
    selectedText: surface("selected-text", pal.accent, 1.0),
    selectedBorder: surface("selected-border", pal.foreground, 0.0),
    fontSize: base,
    fontScale: Math.max(1 / 12, base / 12),
  }
}

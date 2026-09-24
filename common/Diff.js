.pragma library

// Line diff for comparing two clips (Ctrl+F2), as aligned rows for a
// side-by-side view: [{ left, right, kind }] where kind is "same",
// "changed", "added" (right only) or "removed" (left only).
//
// A plain LCS table: fine for clips up to a few thousand lines. Above
// MAX_CELLS the lines are paired by position instead of aligned.

var MAX_CELLS = 4000000

function lines(text) {
  var t = String(text === null || text === undefined ? "" : text).replace(/\r\n?/g, "\n")
  if (t.endsWith("\n")) t = t.slice(0, -1)
  return t.length ? t.split("\n") : []
}

function byPosition(a, b) {
  var rows = []
  for (var i = 0; i < Math.max(a.length, b.length); i++) {
    var l = i < a.length ? a[i] : null, r = i < b.length ? b[i] : null
    rows.push({ left: l, right: r, kind: l === null ? "added" : r === null ? "removed" : l === r ? "same" : "changed" })
  }
  return rows
}

function compare(textA, textB) {
  var a = lines(textA), b = lines(textB)
  var n = a.length, m = b.length
  if ((n + 1) * (m + 1) > MAX_CELLS) return { rows: byPosition(a, b), aligned: false }

  // LCS lengths, bottom-up, in one flat typed array.
  var w = m + 1
  var t = new Uint32Array((n + 1) * w)
  for (var i = n - 1; i >= 0; i--)
    for (var j = m - 1; j >= 0; j--)
      t[i * w + j] = a[i] === b[j] ? t[(i + 1) * w + j + 1] + 1 : Math.max(t[(i + 1) * w + j], t[i * w + j + 1])

  // Walk it, pairing a run of removals with the run of additions right
  // after it as "changed" rows, so an edited line sits beside its original.
  var rows = [], removed = [], added = []
  function flush() {
    var k = 0
    for (; k < Math.min(removed.length, added.length); k++) rows.push({ left: removed[k], right: added[k], kind: "changed" })
    for (var x = k; x < removed.length; x++) rows.push({ left: removed[x], right: null, kind: "removed" })
    for (var y = k; y < added.length; y++) rows.push({ left: null, right: added[y], kind: "added" })
    removed = []
    added = []
  }
  i = 0
  j = 0
  while (i < n && j < m) {
    if (a[i] === b[j]) { flush(); rows.push({ left: a[i], right: b[j], kind: "same" }); i++; j++ }
    else if (t[(i + 1) * w + j] >= t[i * w + j + 1]) removed.push(a[i++])
    else added.push(b[j++])
  }
  while (i < n) removed.push(a[i++])
  while (j < m) added.push(b[j++])
  flush()
  return { rows: rows, aligned: true }
}

function summary(rows) {
  var c = { same: 0, changed: 0, added: 0, removed: 0 }
  for (var i = 0; i < rows.length; i++) c[rows[i].kind]++
  return c
}

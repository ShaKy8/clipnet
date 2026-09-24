import QtQuick
import QtTest
import "../../common/Diff.js" as Diff

TestCase {
  name: "Diff"

  function kinds(r) { return r.rows.map(x => x.kind).join(",") }

  function test_identical() {
    const r = Diff.compare("a\nb\nc", "a\nb\nc")
    compare(kinds(r), "same,same,same")
    verify(r.aligned)
  }
  function test_edit_pairs_lines() {
    const r = Diff.compare("one\ntwo\nthree", "one\n2\nthree")
    compare(kinds(r), "same,changed,same")
    compare(r.rows[1].left, "two")
    compare(r.rows[1].right, "2")
  }
  function test_insert_and_delete() {
    compare(kinds(Diff.compare("a\nc", "a\nb\nc")), "same,added,same")
    compare(kinds(Diff.compare("a\nb\nc", "a\nc")), "same,removed,same")
  }
  function test_empty_sides() {
    compare(kinds(Diff.compare("", "x\ny")), "added,added")
    compare(kinds(Diff.compare("x", "")), "removed")
    compare(Diff.compare("", "").rows.length, 0)
  }
  function test_line_endings_and_trailing_newline() {
    compare(kinds(Diff.compare("a\r\nb\r\n", "a\nb")), "same,same")
  }
  function test_large_falls_back_to_position() {
    const big = []
    for (let i = 0; i < 2100; i++) big.push("line " + i)
    const r = Diff.compare(big.join("\n"), big.join("\n"))
    verify(!r.aligned)
    compare(Diff.summary(r.rows).same, 2100)
  }
  function test_summary() {
    const s = Diff.summary(Diff.compare("a\nb\nc\nd", "a\nB\nc\ne\nf").rows)
    compare(s.same, 2)
    compare(s.changed, 2)
    compare(s.added, 1)
  }
}

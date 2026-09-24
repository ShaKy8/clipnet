import QtQuick
import QtTest
import "../../common/Keys.js" as Keys
import "../../common/Format.js" as Format

TestCase {
  name: "KeysAndFormat"

  function ev(key, mods) { return { key: key, modifiers: mods || 0 } }

  function test_accelerators() {
    compare(Keys.accel(Qt, ev(Qt.Key_1, Qt.ControlModifier | Qt.AltModifier)), "Ctrl+Alt+1")
    compare(Keys.accel(Qt, ev(Qt.Key_V, Qt.MetaModifier | Qt.ControlModifier)), "Super+Ctrl+V")
    compare(Keys.accel(Qt, ev(Qt.Key_Apostrophe, Qt.ControlModifier)), "Ctrl+Apostrophe")
    compare(Keys.accel(Qt, ev(Qt.Key_F5)), "F5")
  }
  function test_shifted_symbols_map_back() {
    // Shift+' arrives as Key_QuoteDbl on a US layout.
    compare(Keys.accel(Qt, ev(Qt.Key_QuoteDbl, Qt.ControlModifier | Qt.ShiftModifier)), "Ctrl+Shift+Apostrophe")
    compare(Keys.accel(Qt, ev(Qt.Key_Exclam, Qt.ControlModifier | Qt.ShiftModifier)), "Ctrl+Shift+1")
  }
  function test_modifiers_alone_are_not_keys() {
    verify(Keys.isModifier(Qt, Qt.Key_Control))
    verify(!Keys.isModifier(Qt, Qt.Key_A))
    compare(Keys.accel(Qt, ev(Qt.Key_Control, Qt.ControlModifier)), null)
  }
  function test_pretty() {
    compare(Keys.pretty("Ctrl+Apostrophe"), "Ctrl+'")
    compare(Keys.pretty("Super+Shift+Grave"), "Super+Shift+`")
  }
  function test_positions_and_age() {
    compare(Format.positionLabel(0), "1")
    compare(Format.positionLabel(8), "9")
    compare(Format.positionLabel(9), "0")
    compare(Format.positionLabel(10), "")
    const now = 10000000000
    compare(Format.age(now - 2000, now), "now")
    compare(Format.age(now - 90 * 1000, now), "1m")
    compare(Format.age(now - 3 * 3600 * 1000, now), "3h")
    compare(Format.size(1536), "1.5 KB")
  }
}

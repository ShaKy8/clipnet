import QtQuick
import QtTest
import "../../common/ThemeParse.js" as Parse

TestCase {
  name: "ThemeParse"

  readonly property string colors: 'accent = "#7d82d9"\nbackground = "#060B1E"\nforeground = "#ffcead"\nred = "#ED5B5A"\n'

  function test_palette_from_colors() {
    const t = Parse.compute(colors, "", "")
    compare(t.accent, "#ff7d82d9")
    compare(t.background, "#ff060b1e")
    compare(t.urgent, "#ffed5b5a")
    compare(t.fontSize, 12)
  }
  function test_menu_tokens_and_references() {
    const shell = '[hyprland]\nactive-border-foreground = "#ffcead"\n[menu]\nbackground = "#060B1E"\nbackground-alpha = 0.5\n' +
                  'border = "hyprland.active-border-foreground"\nselected-background = "#ffcead"\nselected-background-alpha = 0.08\n'
    const t = Parse.compute(colors, shell, "")
    compare(t.card, "#80060b1e")
    compare(t.border, "#ffffcead")
    compare(t.selectedBackground, "#14ffcead")
  }
  function test_gradient_uses_first_stop() {
    const shell = '[hyprland]\nactive-border = "rgba(33ccffee) rgba(00ff99ee) 45deg"\n[menu]\nborder = "hyprland.active-border"\n'
    compare(Parse.compute(colors, shell, "").border, "#ee33ccff")
  }
  function test_user_override_wins_and_font() {
    const t = Parse.compute(colors, '[font]\nbase-size = 12\n', '[font]\nbase-size = 15\n')
    compare(t.fontSize, 15)
    compare(t.fontScale, 1.25)
  }
  function test_legacy_theme_without_named_colors() {
    const t = Parse.compute('color0 = "#000000"\ncolor4 = "#0000ff"\ncolor7 = "#ffffff"\n', "", "")
    compare(t.background, "#ff000000")
    compare(t.accent, "#ff0000ff")
    compare(t.foreground, "#ffffffff")
  }
  function test_role_names_and_comments() {
    const t = Parse.compute(colors, '[menu]\ntext = accent  # inline comment\n', "")
    compare(t.text, "#ff7d82d9")
  }
}

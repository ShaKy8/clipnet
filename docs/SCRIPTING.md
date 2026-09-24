# Scripting CLIP//NET

Ditto can run scripts when clips are copied and pasted. CLIP//NET does the
same with [Lua 5.4](https://www.lua.org/manual/5.4/).

A script is a `.lua` file in `~/.config/clipnet/scripts/`. It can define
`on_copy`, `on_paste`, or both. Every script starts **disabled**: turn it on in
Settings → Scripts, which can also test it against a clip you already have.
Scripts run in name order, so prefix them (`10-…`, `20-…`) when order matters.

The bundled examples are a good starting point: **Settings → Scripts →
Install examples** copies them in, disabled, and never overwrites a file of
yours.

| Example | What it does |
|---|---|
| `strip-tracking.lua` | removes `utm_*`, `fbclid`, `gclid`, … from copied links |
| `skip-otp.lua` | doesn't store one-time codes (4–8 digits on their own) |
| `git-to-group.lua` | files git URLs and `git clone` commands under Code ▸ Git |
| `terminal-safe-paste.lua` | drops the trailing newline when pasting into a terminal, so commands wait for Enter |

## on_copy(clip)

Runs for every new copy, after rules, before the clip is stored.

```lua
function on_copy(clip)
  if clip.text and clip.text:match("^%d%d%d%d%d%d$") then
    return false                                  -- don't store it
  end
  if clip.source.app == "org.mozilla.firefox" then
    return { group = "Web", title = "from Firefox" }
  end
  -- return nothing to store the clip as it is
end
```

**Returns** one of:

- `nil`, or `true`: store the clip unchanged.
- `false`: don't store it. The copy itself still works; it just isn't kept.
- A table with any of:

  | key | |
  |---|---|
  | `text` | Replace the text. The clip becomes plain text, because its other formats (HTML, images) no longer match. |
  | `title` | A description, shown instead of the text. |
  | `group` | `"Parent/Child"`. Created if it doesn't exist. |
  | `sticky` | `true` pins the clip to the top. |
  | `locked` | `true`: never auto-deleted. |

When several scripts run, each sees the text the previous one returned. A
`false` from any of them stops the rest.

## on_paste(clip, target)

Runs just before a paste into a window, whether from Enter, Ctrl+1–0, or a
hotkey. It doesn't run when you only copy a clip (Ctrl+C in the popup), because
there is no target window yet. It also doesn't run for Special Paste or copy
buffers, where you have already chosen exactly what to paste.

```lua
function on_paste(clip, target)
  if target.app:find("ghostty") then
    return (clip.text:gsub("\n$", ""))           -- paste this instead
  end
end
```

Return a string to paste it instead, as plain text, or `nil` to paste as usual.
When several clips are pasted together, `clip.text` is their joined text.

## The clip

| field | |
|---|---|
| `clip.text` | The text, or `nil` for a clip without any (an image) |
| `clip.kind` | `"text"`, `"rich"`, `"image"`, `"files"` or `"other"` |
| `clip.mimes` | The formats, e.g. `{ "text/plain;charset=utf-8", "text/html" }` |
| `clip.size` | Total bytes across formats |
| `clip.title` | Its description, if it has one |
| `clip.source.app` | The class of the window that had focus when it was copied (`on_copy` only) |
| `clip.source.title` | That window's title (`on_copy` only) |
| `clip.get(mime)` | The raw bytes of one format, e.g. `clip.get("text/html")`. Only valid during the call. |
| `target.app` | The class of the window being pasted into (`on_paste` only) |

App classes are what `hyprctl clients` lists: `com.mitchellh.ghostty`,
`chromium`, `org.mozilla.firefox`, and so on.

## Logging

`clipnet.log(...)` (and `print`) writes to the daemon's log, which you can
read with `journalctl --user -u clipnetd`. A call can write at most 20 lines.
In a Settings dry run the lines show in the result instead. Anything you log
ends up in the journal, so avoid logging clip text you wouldn't want there.

## The sandbox

Scripts get Lua's `string`, `table`, `math`, `utf8` and `coroutine` libraries,
plus `os.time`, `os.date`, `os.clock` and `os.difftime`. There is no `io`,
`os.execute`, `require`, `dofile`, `loadfile` or `debug`, and `load` only
accepts source text. Each script has its own Lua state with 64 MiB of memory,
and each call has 100 ms. A script that errors, runs out of time or runs out
of memory is skipped for that clip: the copy or paste goes on as if it weren't
there, and the error shows in Settings → Scripts.

Files are reloaded automatically when they change. **Reload** in Settings
forces it.

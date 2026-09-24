# clipnetd protocol (v1)

JSON lines over a Unix socket at `$XDG_RUNTIME_DIR/clipnet/clipnetd.sock`.
The directory is 0700, and only clients running as the daemon's own uid are
accepted (`SO_PEERCRED`). One JSON object per line, UTF-8.

Nothing in the protocol is specific to Wayland or Hyprland, so a future macOS
daemon can speak it unchanged. Where a request can't be served on a platform
it fails with an error code instead of changing shape.

## Framing

```
→ {"id": 7, "op": "list", "query": "curl", "limit": 50}
← {"id": 7, "ok": true, "result": {"rows": [...], "total": 3}}
← {"id": 8, "ok": false, "error": {"code": "not_found", "message": "no such clip"}}
← {"event": "clip.added", "data": {...row...}}
```

- Every request gets exactly one reply, in request order. `id` is echoed back
  as given; it may be any JSON value.
- Events arrive only after `hello` with `"subscribe": true`, and never carry an `id`.
- There is no binary data on the socket. Image and large payloads are referred
  to by absolute file path (`image`, `formats[].path`).
- Lines are limited to 16 MiB. A client that stops reading is dropped once
  64 MiB of output is queued for it.

`clipnetd --ctl '<json>'` sends one request and prints the result. It exits 1
on an error reply and 2 if the daemon can't be reached. `scripts/clipnet`
is built on it.

## Clip rows

Returned by `list`, `get`, and the `clip.added` / `clip.updated` events:

| Field | |
|---|---|
| `id`, `uuid` | local id; uuid for export/sync |
| `kind` | `text`, `rich`, `image`, `files`, `other` |
| `preview` | one line, ≤ 300 characters |
| `title` | the user's description, or null |
| `created_at`, `last_used_at` | Unix ms UTC |
| `paste_count`, `size` | |
| `source_app`, `source_title` | focused app when copied (title only if `store_source_title`) |
| `sticky`, `locked`, `group_id`, `quick_paste`, `hotkey` | organisation (Phase 2) |
| `mimes` | stored formats, in offer order |
| `image` | path of the image payload, or null |
| `flags` | bit 1: the source offered its HTML markup as "plain text" |

`get` also returns `formats` (`[{mime, size, aliases, path?}]`) and `text`
(the full text, or null).

## Operations

| op | arguments | result |
|---|---|---|
| `hello` | `client`, `subscribe` | `{proto, version, wayland, hyprland, paused}` |
| `ping` | | `"pong"` |
| `show_context` | | `{cursor{x,y}, monitor{name,x,y,width,height,scale}, app, rounding}`; also re-checks keep-alive |
| `list` | `query`, `group`, `all_groups`, `offset`, `limit` (≤ 5000) | `{rows, total}` |
| `get` | `id` | row + `formats` + `text` |
| `stats` | | `{count, content_bytes, db_bytes, schema}` |
| `paste` | `ids` or `id`, `mode` (`normal`\|`plain`), `separator`, `text` | null. Puts the clips on the clipboard and presses paste in the focused window |
| `copy` | same as `paste` | null. Clipboard only, no keystroke |
| `paste_position` / `copy_position` | `n` (1 = newest in history order) | null |
| `create` | `text`, `title` | `{id, duplicate}` |
| `delete` | `ids` or `id` | `{deleted}` |
| `state.get` | | `{paused, paused_until, current, count}` |
| `pause` | `minutes` (0 = until resumed, ≤ 10080) | null |
| `resume` | | null |
| `settings.get` | | every setting with its value |
| `settings.set` | `key`, `value` | null (`bad_value` on a wrong type or range) |
| `import` | `format: "omarchy"`, `path` | `{added, duplicates, skipped}` |
| `retention.run` | | `{removed}` |

When `paste` or `copy` gets several ids, the clips' text is joined with
`separator` (default: the `multi_separator` setting) and pasted as a single
text clip. `text` overrides the content, for Special Paste transforms, while
still crediting the ids.

## Events

`clip.added`, `clip.updated` (a row), `clip.deleted` (`{id}`), `clips.reset`
(reload everything: after an import or retention), `state.changed`
(`{paused, paused_until}`), `settings.changed` (`{key, value}`).

## Error codes

`bad_request`, `unknown_op`, `not_found`, `bad_value`, `paste_failed`,
`db_error`, `import_failed`, `too_large`.

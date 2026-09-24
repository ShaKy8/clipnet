#!/usr/bin/env python3
"""IPC round trips against a throwaway clipnetd in --no-wayland mode.

Covers the protocol surface that needs no compositor: listing and search,
editing, groups, sticky ordering, transforms, export/import, settings,
errors, and event delivery. Standard library only.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.environ.get("CLIPNETD", os.path.join(ROOT, "daemon", "clipnetd"))

failures = 0
checks = 0


def check(what, cond):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL {what}")


class Client:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.buf = b""
        self.next = 1
        self.events = []

    def _line(self, timeout=3.0):
        self.s.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("daemon closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def call(self, op, **args):
        rid = self.next
        self.next += 1
        self.s.sendall((json.dumps({**args, "rid": rid, "op": op}) + "\n").encode())
        while True:
            msg = self._line()
            if "event" in msg:
                self.events.append(msg)
                continue
            assert msg["rid"] == rid, msg
            return msg

    def ok(self, op, **args):
        msg = self.call(op, **args)
        if not msg["ok"]:
            raise AssertionError(f"{op} failed: {msg['error']}")
        return msg["result"]


def main():
    work = tempfile.mkdtemp(prefix="clipnet-ipc.")
    sockdir = tempfile.mkdtemp(prefix="clipnet-ipc.", dir=os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
    sock = os.path.join(sockdir, "s.sock")
    log = open(os.path.join(work, "log"), "w")
    # Its own config dir: scripts must never land in the user's real one.
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(work, "config"))
    proc = subprocess.Popen([BIN, "--no-wayland", "--data", os.path.join(work, "data"), "--socket", sock, "-v"],
                            stderr=log, env=env)
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.02)
        c = Client(sock)
        watcher = Client(sock)
        watcher.ok("hello", client="test", subscribe=True)

        hello = c.ok("hello", client="test")
        check("hello reports no wayland", hello["wayland"] is False)

        ids = [c.ok("create", text=t)["id"] for t in ("alpha one", "bravo two", "charlie three", "delta four")]
        check("created four", len(set(ids)) == 4)
        time.sleep(0.01)  # timestamps are milliseconds; make "later" unambiguous
        check("duplicate create is detected", c.ok("create", text="alpha one")["duplicate"] is True)

        # Copying something again moves it to the top (as in Ditto).
        rows = c.ok("list")["rows"]
        check("re-copied clip moves to the top", [r["preview"] for r in rows][:2] == ["alpha one", "delta four"])
        check("search", [r["preview"] for r in c.ok("list", query="two")["rows"]] == ["bravo two"])

        # Editing
        r = c.ok("update", id=ids[0], title="First", quick_paste="first", locked=True)
        check("update returns the row", r["title"] == "First" and r["locked"] and r["quick_paste"] == "first")
        bad = c.call("update", id=ids[1], quick_paste="first")
        check("quick-paste word clash is refused", not bad["ok"] and "already" in bad["error"]["message"])
        bad = c.call("update", id=ids[1], sticky="sideways")
        check("bad sticky value is refused", not bad["ok"])
        c.ok("update", id=ids[1], sticky="top")
        check("sticky goes first", c.ok("list")["rows"][0]["id"] == ids[1])
        c.ok("update", id=ids[1], sticky=False)
        r = c.ok("set_text", id=ids[2], text="charlie EDITED")
        check("set_text", r["preview"] == "charlie EDITED" and r["kind"] == "text")
        check("set_text to a duplicate is refused", not c.call("set_text", id=ids[2], text="delta four")["ok"])

        # Groups
        g = c.ok("groups.create", name="Work")["id"]
        sub = c.ok("groups.create", name="Snippets", parent=g)["id"]
        check("duplicate group name refused", not c.call("groups.create", name="Work")["ok"])
        check("cycle refused", not c.call("groups.move", id=g, parent=sub)["ok"])
        c.ok("move", ids=[ids[0], ids[1]], group=sub)
        check("group scope", sorted(r["id"] for r in c.ok("list", group=sub)["rows"]) == sorted(ids[:2]))
        groups = {x["name"]: x for x in c.ok("groups.list")}
        check("group counts", groups["Snippets"]["count"] == 2 and groups["Work"]["count"] == 0)
        c.ok("groups.rename", id=sub, name="Code")
        check("renamed", any(x["name"] == "Code" for x in c.ok("groups.list")))

        # Transforms
        names = [t["id"] for t in c.ok("transforms.list")]
        check("transform list", "upper" in names and "slug" in names)
        pr = c.call("paste", id=ids[0], transform="upper")
        check("paste without wayland fails cleanly", not pr["ok"] and pr["error"]["code"] == "paste_failed")

        # Export / import
        path = os.path.join(work, "export.json")
        ex = c.ok("export", path=path)
        check("export counts", ex["clips"] == 4 and ex["groups"] == 2)
        check("export is private", (os.stat(path).st_mode & 0o777) == 0o600)
        check("relative export path refused", not c.call("export", path="rel.json")["ok"])
        im = c.ok("import", format="clipnet-json", path=path)
        check("re-import is all duplicates", im["added"] == 0 and im["duplicates"] == 4)

        # Deleting a group returns its clips to history
        released = c.ok("groups.delete", id=g)
        check("group delete releases clips", released["released_clips"] == 2)
        check("clips kept", len(c.ok("list")["rows"]) == 4)
        check("subgroup gone too", c.ok("groups.list") == [])

        # Settings and errors
        check("unknown setting", c.call("settings.set", key="nope", value=1)["error"]["code"] == "bad_value")
        c.ok("settings.set", key="popup_rows", value=20)
        check("setting stored", c.ok("settings.get")["popup_rows"] == 20)
        check("unknown op", c.call("frobnicate")["error"]["code"] == "unknown_op")
        check("bad id type", c.call("get", id="seven")["error"]["code"] == "bad_request")
        check("missing clip", c.call("get", id=999999)["error"]["code"] == "not_found")
        c.s.sendall(b"not json\n")
        check("garbage line gets an error", c._line()["error"]["code"] == "bad_request")

        # Rules (Phase 3)
        seeded = c.ok("rules.list")
        check("password managers excluded by default", any(r["match_app"] == "*keepassxc*" for r in seeded))
        r1 = c.ok("rules.set", action="exclude", match_app="*slack*")["id"]
        bad = c.call("rules.set", action="paste_keys", match_app="*kitty*", arg={"keys": "Ctrl+Nope"})
        check("bad paste keys refused", not bad["ok"])
        r2 = c.ok("rules.set", action="paste_keys", match_app="*kitty*", arg={"keys": "shift+ctrl+v"})["id"]
        rules = {r["id"]: r for r in c.ok("rules.list")}
        check("paste keys stored canonically", rules[r2]["arg"]["keys"] == "Ctrl+Shift+V")
        c.ok("rules.set", id=r1, action="exclude", match_app="*slack*", enabled=False)
        check("rule disabled", not {r["id"]: r for r in c.ok("rules.list")}[r1]["enabled"])
        c.ok("rules.reorder", ids=[r2, r1])
        check("reordered", [r["id"] for r in c.ok("rules.list")][:2] == [r2, r1])
        c.ok("rules.delete", id=r1)
        check("rule deleted twice fails", not c.call("rules.delete", id=r1)["ok"])

        # Copy buffers (need a compositor to act; state works without one)
        bufs = c.ok("buffers.get")
        check("three empty buffers", [b["slot"] for b in bufs] == [1, 2, 3] and all(b["clip"] is None for b in bufs))
        check("paste from an empty buffer fails", not c.call("buffer.paste", slot=2)["ok"])
        check("buffer 4 does not exist", not c.call("buffer.copy", slot=4)["ok"])
        hk = c.ok("hotkeys.set", accel="Ctrl+Alt+F9", action="buffer_paste", arg="2")["id"]
        check("buffer hotkey refuses slot 5", not c.call("hotkeys.set", accel="Ctrl+Alt+F10", action="buffer_copy", arg="5")["ok"])
        pt = c.ok("hotkeys.set", accel="Ctrl+Alt+F8", action="pause_toggle")["id"]
        labels = {h["id"]: h["label"] for h in c.ok("hotkeys.list")}
        check("hotkey labels", labels.get(hk) == "paste buffer 2" and labels.get(pt) == "pause or resume recording")
        c.ok("hotkeys.remove", id=hk)
        c.ok("hotkeys.remove", id=pt)

        # Scripts (Phase 4)
        sl = c.ok("scripts.list")
        check("scripts dir is the private one", sl["dir"] == os.path.join(work, "config", "clipnet", "scripts"))
        check("no scripts yet", sl["scripts"] == [])
        ex = c.call("scripts.install_examples")
        if ex["ok"]:
            check("examples installed", len(ex["result"]["added"]) == 4)
            names = [x["name"] for x in c.ok("scripts.list")["scripts"]]
            check("examples listed, all disabled", "skip-otp.lua" in names and
                  not any(x["enabled"] for x in c.ok("scripts.list")["scripts"]))
            c.ok("scripts.enable", name="skip-otp.lua", enabled=True)
            check("enabled", any(x["name"] == "skip-otp.lua" and x["enabled"] for x in c.ok("scripts.list")["scripts"]))
            otp = c.ok("create", text="482913")["id"]
            t = c.ok("scripts.test", name="skip-otp.lua", hook="on_copy", id=otp)
            check("dry run says skip", t["result"]["skip"] is True)
            c.ok("scripts.enable", name="skip-otp.lua", enabled=False)
            c.ok("delete", ids=[otp])
        else:
            check("examples missing only for a relocated binary", "not found" in ex["error"]["message"])
        check("unknown script refused", not c.call("scripts.enable", name="nope.lua", enabled=True)["ok"])

        vac = c.ok("vacuum")
        check("vacuum reports sizes", vac["before"] > 0 and vac["after"] > 0)
        r = c.ok("list", query="alpha")
        check("search reports whether it stopped counting", r["more"] is False and r["total"] >= 1)

        deleted = c.ok("delete", ids=[ids[3]])
        check("delete", deleted["deleted"] == 1)

        # Events arrived for the subscribed client only
        time.sleep(0.1)
        try:
            while True:
                watcher.events.append(watcher._line(timeout=0.2))
        except (socket.timeout, TimeoutError):
            pass
        kinds = {e["event"] for e in watcher.events}
        for k in ("clip.added", "clip.updated", "clip.deleted", "groups.changed", "settings.changed", "clips.reset"):
            check(f"event {k} delivered", k in kinds)
        check("unsubscribed client got no events", c.events == [])
    finally:
        proc.terminate()
        rc = proc.wait(timeout=5)
        log.close()
        check("daemon exits cleanly", rc == 0)
        if failures:
            print(open(os.path.join(work, "log")).read())
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(sockdir, ignore_errors=True)
    print(f"{'FAIL' if failures else 'ok  '} ipc_test ({checks} checks{', %d failed' % failures if failures else ''})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

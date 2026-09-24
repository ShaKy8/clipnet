<!-- Thanks for contributing! See CONTRIBUTING.md for the details behind this checklist. -->

## What and why

<!-- What this changes, and the problem it solves. Link an issue if there is one ("Fixes #12"). -->

## How it's tested

<!-- The tests you added or changed, and anything you checked by hand (which apps, which keys). -->

## Checklist

- [ ] One change, with tests that fail without it
- [ ] `make test` and `make asan` pass (CI runs them, plus the live test)
- [ ] `make test-live` passes, if this touches capture, serving, pasting or keep-alive
- [ ] Clip contents never reach a log
- [ ] Docs updated where needed: `README.md` (keys, features), `docs/PROTOCOL.md` (operations, events), `docs/SCHEMA.md` (migrations, which are append-only), `docs/SCRIPTING.md` (script API)
- [ ] Screenshots, if any, use demo data (`tools/readme-screenshot.sh`), never a real history

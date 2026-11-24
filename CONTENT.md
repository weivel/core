## What this is

The weivel plugin engine, written in C. It decides, once for every language that wraps it, whether a plugin is
compatible, the order plugins activate in, what each may reach, how its wants settle, what is revoked when it stops,
and what the ledger records about it.

## What it is not

It decides and does nothing else: it never calls out, owns no clock, opens no file and holds no plugin object, only
integer handles the caller maps to its own. Loading plugins, reading manifests, calling services and logging are the
caller's work.

## Where to read on

Start at [docs/README.md](docs/README.md), the core's architecture and the full statement of what it is and is not,
and descend: the conventions every call keeps, the ledger, the contract a language implementation follows, and one
doc per source folder, where building and testing the core is answered.

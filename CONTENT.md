## What this is

The weivel plugin engine, written in C. It decides: whether a plugin is compatible with a host, the order plugins
activate in, which service or capability a plugin may reach and who provides it, when a wait settles or is refused,
what is revoked when a plugin stops, and what the ledger records about every plugin. A language implementation wraps
it, and every language sees the same decisions because they are made here once.

## What it is not

It decides and does nothing else. It never calls out: no callback, no listener, no function pointer is ever invoked
by it. It owns no clock, opens no file, reads no environment variable and parses no format, so a manifest reaches it
as a structure the caller builds, never as text. It holds no plugin object, service implementation or listener:
each is an integer handle the caller maps to its own object, and a handle whose object is gone is refused as data,
never mistaken for a newer one. Loading plugins from disk, reading manifests, calling services and logging are the
caller's work.

## Where to read on

Start at [docs/README.md](docs/README.md), the core's architecture, and descend: the conventions every call keeps,
the ledger, the contract a language implementation follows, and one doc per source folder, where building and testing
the core is answered.

The weivel plugin engine, written in C. It decides: whether a plugin is compatible, the order plugins activate in,
which service or capability a plugin may reach and who provides it, when a wait settles, and what the ledger records.
It takes every input as a value through its C API, never calls out, reads no clock, file or environment variable,
and holds plugins only as integer handles. A language implementation wraps it.

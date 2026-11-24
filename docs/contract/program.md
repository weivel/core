# `program` (optional)

A host program in the language: it runs any C library serving the standard's `weivel.host` (`host.h`), from a
command line and as a function, and gives each plugin what the standard's `weivel.scope` answers for it.

- It carries out `host.h` and nothing more: every decision of a run is the host library's answer, and the program
  reads the clock and passes it in.
- It stands on the host and the standard: it loads libraries and plugins through the host's public surface and
  parses every file through the standard.
- A repository may omit this module; the kit's `host/` layer vectors bind only a language that ships one.
- It may not decide an order, a mode, an exit code or a plugin's services of its own.

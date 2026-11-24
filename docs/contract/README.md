The contract a language implementation follows. A language implementation wraps the core so that plugins written in its language can use it. Every implementation has the modules `MODULES` lists, under the same names, and each module does what this folder says it does and nothing it says it may not. The rules are written here once, so a new language translates them rather than copying what an existing implementation happens to do. [The core's own docs](../README.md) describe the core itself; this folder describes what stands on it. Where a rule can be witnessed only from outside a layer, the conformance kit carries a layer vector for it, named here by its id (its path under the kit's `vectors/layer`, without `.json`), and the kit's `docs/vectors/layer/` says what a suite must show for it. A rule with no vector is held by review and by the language's own tests. `MODULES` sits at the root of this repository, and the kit's parity check reads it, its form stated in the kit's `docs/checks.md`; each module has one file below, in the order `MODULES` lists them, after the rules that bind every module.

- [cross-cutting.md](cross-cutting.md) - the rules every module keeps: calls in and drains, the provider's own object, no JSON across a C boundary, sizes, errors to a sink, and dispatch order
- [core.md](core.md) - `core`: loads the core's binary and owns the one wrapper every call goes through, its deadline check, drain, refusals and lock
- [handles.md](handles.md) - `handles`: the table from the core's handles to the language's objects, holdings included
- [timer.md](timer.md) - `timer`: arms the core's next deadline on the language's own timer, and the vectors that witness it
- [context.md](context.md) - `context`: what a plugin sees of the core, from services and capabilities to events, log and manifest
- [host.md](host.md) - `host`: registration, a plugin's entry, activation, release, destroy, and C libraries as host services
- [standard.md](standard.md) - `standard` (optional): the language's access to the standard's defaults
- [conformance.md](conformance.md) - `conformance`: the driver for the layer suite
- [benchmarks.md](benchmarks.md) - `benchmarks`: the language's half of the platform's benchmarks and the gates it holds
- [program.md](program.md) - `program` (optional): a host program that runs any C library serving the standard's `weivel.host`
- [bundle.md](bundle.md) - `bundle` (optional): the packer of plugin entries and the host program, for bundled source
- [testing.md](testing.md) - `testing` (optional): helpers for the tests of a program that hosts plugins

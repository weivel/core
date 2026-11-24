The core's tests: one C program per area, each a ctest of its own with a 60 second timeout, plus CMake scripts and two programs built for others. A test links whichever library proves its point: the static library for what is reached through the internal headers, the shared library for what a foreign function interface sees through the exports alone, and the testing library for what needs an allocation to fail on demand, which is how the out-of-memory tests prove that a call which runs out changes nothing. Every test reports through `check.h`, counting failures rather than stopping at the first, and the registry tests share `registry_support.h`, which drains every event into a journal and registers and opens plugins from a short spec. `echo` and the layouts program are built here because a language implementation's tests need them beside the core they are pinned to; how to run the tests is in [the source mirror's root doc](../README.md).

<!-- generated: source-children -->
- [CMakeLists.txt](../../../test/CMakeLists.txt) - every test and the library it links, `echo` and its unload test, the layouts program and its checks, and the ASCII check
- [abi_test.c](../../../test/abi_test.c) - the size convention through the shared library: older and newer callers' options and events
- [activation_test.c](../../../test/activation_test.c) - the activation plan: order by services and capabilities, optional and degradable requirements, missing ones and cycles
- [ascii_check.cmake](../../../test/ascii_check.cmake) - refuses a public header holding any byte outside ASCII
- [capabilities_test.c](../../../test/capabilities_test.c) - providing, verifying, listing and watching capability implementations, and what a stop takes away
- [check.h](../../../test/check.h) - the `CHECK` macro and text comparison every test reports through
- [compatibility_test.c](../../../test/compatibility_test.c) - the application and api axes of compatibility, and their order
- [concurrency_test.c](../../../test/concurrency_test.c) - two threads hammering one core lose and damage nothing
- [constraints_test.c](../../../test/constraints_test.c) - constrained wants: live and advertised attributes, the polls, serving, degrading and refusing
- [deadline_test.c](../../../test/deadline_test.c) - the deadline announcements: when one is queued, moved or silent
- [echo/](echo/) - `echo`, the neutral C library a language layer's tests bind as a host service
- [echo_test.c](../../../test/echo_test.c) - every `echo` function and definition, through the library's exports
- [echo_unload_test.c](../../../test/echo_unload_test.c) - loads `echo` as a binary and proves a definition counts the closing of its binary
- [events_test.c](../../../test/events_test.c) - taking back the newest event keeps a waiting deadline announcement and the older events in order
- [handle_state_test.c](../../../test/handle_state_test.c) - the condition of wants and watches, live, settled and ended
- [handles_test.c](../../../test/handles_test.c) - handles: stale ones refused after reuse, zero never issued, kinds kept apart, slots retired at their last generation
- [instance_test.c](../../../test/instance_test.c) - creating and destroying a core, malformed calls answered and never crashed, and the queue's order and ownership
- [layouts/](layouts/) - the layouts program and the checks that hold it to every struct the header declares
- [ledger_test.c](../../../test/ledger_test.c) - the ledger: registration order, relationships, quarantine reasons, forgetting and the order of refusals
- [lifecycle_test.c](../../../test/lifecycle_test.c) - churn leaves nothing behind, and a call that cannot hold what it hands over changes nothing
- [manifest_test.c](../../../test/manifest_test.c) - registering a manifest: the core's own copy, defaults, older and newer shapes, and malformed ones
- [oom_test.c](../../../test/oom_test.c) - each call failed at every one of its allocations in turn, proving each failure leaves the core as it was
- [reach_test.c](../../../test/reach_test.c) - reach: declared ids, named providers, late registrations, and refusals reported once
- [registry_support.h](../../../test/registry_support.h) - the event journal and plugin builders the registry tests share
- [registry_test.c](../../../test/registry_test.c) - the service registry: wants with and without deadlines, gets, watches, holdings and revocation
- [releases_test.c](../../../test/releases_test.c) - registrations awaiting releases: holdings, their release, and the one notice to the owner
- [sized_test.c](../../../test/sized_test.c) - the size convention's read and write at the byte level
- [subject_test.c](../../../test/subject_test.c) - the subject every refusal names
- [threads_test.c](../../../test/threads_test.c) - threads meeting and parting on one core leave it consistent
<!-- end generated -->

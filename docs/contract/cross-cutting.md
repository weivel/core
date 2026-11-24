# Cross-cutting rules

**The core never calls out, so the layer calls in and drains.** Every call into the core goes through the one
wrapper the `core` module owns. Nothing else calls the core's binary, and nothing else drains its queue.

**A resolved service is the provider's own object.** A consumer that resolves a service, by a get, a settled want or
a watch hearing it registered, receives exactly the object the provider registered: no proxy, no copy, no wrapper.
Calling it is a native call in the language, with no crossing into the core, no lock taken and no argument copied.
The proof is identity, in every language: pointer equality in C, reference identity elsewhere. The benchmarks gate
it (see [`benchmarks`](benchmarks.md)).

**No JSON crosses a C boundary.** The layer hands the core structures and reads structures back, and a C library's
functions take bytes. A JSON document enters C only through the standard's readers, which a host may never call.

**Nothing assumes a handle's object lives in the caller's language.** A handle is an integer the core issued. The
layer maps it to an object of its own and compares handles only for equality.

**Sizes follow the core's convention.** Every structure the layer hands in carries the size the layer was compiled
against, and every array of growing elements the core hands out is stepped through at the stride its first element
declares, never at the layer's own. A layer built against an older header keeps working against a newer core.

**What goes wrong in code the layer calls goes to a sink, never to a caller.** A listener, a plugin's `deactivate`, a
capacity answer, an unload or a call the timer makes can each throw or fail. The layer catches it and reports it to an
error sink the host's caller may replace, and carries on. It never surfaces as the failure of the call whose drain set
it off: a caller handed an error in place of the registration or plugin it asked for could never release it, and a
drain driven by a timer would end the process. A sink that itself fails is reported to the language's standard error
output. The one exception, here and wherever else this document keeps a failure from a caller, is a fatal runtime
error, one after which the runtime itself cannot be trusted to go on, such as Java's `VirtualMachineError`: the layer
restores its own state and then lets it propagate.

**A text that cannot cross a C boundary whole is the caller's mistake.** A text the caller hands with an embedded NUL
is refused before the core is called, answered to the caller and reported to no sink.

**Events are dispatched in the core's order, outside the core, and the dispatcher is never re-entered.** The wrapper
drains a call's events completely before it dispatches any of them, and dispatches them first in, first out. A call
made while an event is dispatched drains its own events too, and they join the end of what is already drained: they
are dispatched after every event drained before them, never ahead of one, which is the core's "events come out in the
order they happened". So no listener hears of a handle after a later change has already ended it. The timer is the
one reader that acts at drain time rather than at dispatch (see [`timer`](timer.md)).

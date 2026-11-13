# The contract a language implementation follows

A language implementation wraps the core so that plugins written in its language can use it. Every implementation
has the modules `MODULES` lists, under the same names, and each module does what this document says it does and
nothing it says it may not. The rules are written here once, so a new language translates them rather than copying
what an existing implementation happens to do. `CONTENT.md` describes the core itself; this document describes what
stands on it.

Where a rule can be witnessed only from outside a layer, the kit carries a layer vector for it, named here by its id
(its path under the kit's `vectors/layer`, without `.json`), and `vectors/layer/LAYER.md` in the kit says what a
suite must show for it. A rule with no vector is held by review and by the language's own tests.

## Cross-cutting rules

**The core never calls out, so the layer calls in and drains.** Every call into the core goes through the one
wrapper the `core` module owns. Nothing else calls the core's binary, and nothing else drains its queue.

**A resolved service is the provider's own object.** A consumer that resolves a service, by a get, a settled want or
a watch hearing it registered, receives exactly the object the provider registered: no proxy, no copy, no wrapper.
Calling it is a native call in the language, with no crossing into the core, no lock taken and no argument copied.
The proof is identity, in every language: pointer equality in C, reference identity elsewhere. The benchmarks gate
it (see `benchmarks`).

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
output.

**Events are dispatched in the core's order, outside the core, and the dispatcher is never re-entered.** The wrapper
drains a call's events completely before it dispatches any of them, and dispatches them first in, first out. A call
made while an event is dispatched drains its own events too, and they join the end of what is already drained: they
are dispatched after every event drained before them, never ahead of one, which is the core's "events come out in the
order they happened". So no listener hears of a handle after a later change has already ended it. The timer is the
one reader that acts at drain time rather than at dispatch (see `timer`).

## `core`

Loads the core's binary and owns the one wrapper every call goes through.

- The wrapper makes the call, then drains: it calls `weivel_next_event` while the core reports events waiting, taking
  the count first from the call's own `pending_events` and then from each `weivel_next_event`, and stops when the
  count is 0 or the core answers `WEIVEL_STATUS_NONE`. A call that reports none waiting costs no further crossing.
- Each event is copied into the language's own values and handed to `weivel_event_release` before the next is
  drained, whatever the copy does, so the core never meets an unreleased event. An event the layer cannot copy is
  reported to the error sink and the rest are still drained.
- A refused call is answered to the caller with the refusal the core queued for it, read from the drained events.
  Where the core queues none, the layer answers the status alone, with the reason the core's documentation gives
  for that case: a refusal of reach already reported once for that plugin and id is a repeat and answers
  `WEIVEL_STATUS_REFUSED` with no new refusal; a registration or watch from a plugin no longer running is a late call
  and queues only a log; `weivel_handle_state` refuses a handle that names nothing with no event at all. The layer
  never invents a refusal the core did not give.
- `WEIVEL_STATUS_MALFORMED` from a call the layer built is a defect in the layer and is reported as one, never as the
  caller's mistake.
- A call and its drain are one step against every other call: the events drained after a call are that call's, and
  no other thread's call may land between them. A language with several threads either holds one layer lock across
  the call and its drain, or makes every call on one owning thread that others hand their calls to. One thread drains.
- It may not keep a list of which calls move a deadline, interpret a handle, or call any core function outside the
  wrapper.

## `handles`

The table from the core's handles to the language's objects: plugins, service registrations, implementations of
capabilities, watches and wants.

- An entry is made when the core answers the handle and ends when what the handle stands for ends: an unregistration,
  a withdrawn implementation, a stopped watch, a settled or withdrawn want, a released or forgotten plugin. A
  `WEIVEL_EVENT_REVOKED` whose `plugin` names a forgotten plugin tells the layer which of that plugin's entries to drop
  and is never used as a handle to call with.
- A handle the table does not hold answers nothing. It is never matched to a newer object, because the core never
  reissues a handle while its object lives and refuses a stale one as data.
- A plugin may be opened with the layer's own token as its payload (`weivel_open_plugin`), so the layer reaches its
  record from a plugin handle without a search.
- It may not hold an object the core has stopped naming, nor answer one plugin's object to another unless the core
  answered that handle to it.

## `timer`

Arms the core's next deadline on the language's own timer.

- The timer is armed from the latest `WEIVEL_EVENT_DEADLINE` drained, and from nothing else: `set` 1 arms it for
  `at`, replacing any deadline armed before, and `set` 0 disarms it. It is armed at drain time, as the wrapper drains
  the event, never later when events are dispatched, because a dispatch can run after a newer announcement has
  already been drained. Because the core keeps at most one announcement waiting and queues each after every other
  event of its call, the latest one drained is always the current deadline.
- When the timer fires it hands the core the time through the wrapper with `weivel_advance(now)`. The events that
  call queues re-arm or disarm it as any others do.
- A language whose timer cannot wait as long as a deadline lies away waits as long as it can and arms again when that
  wait ends, never calling `weivel_advance` before the deadline it armed.
- When the deadline and other work fall due at the same moment, the timer fires first: the layer hands the core that
  time before it makes the other call, so a registration at a want's deadline settles nothing.
- With no deadline there is no timer, so a want that asked for no timeout waits for as long as it takes.
- It may not keep a list of the calls that move a deadline, poll the core for its deadline, or advance the core on a
  time of its own reckoning.

Witnessed by `want/a-timed-want-arms-the-timer-at-its-deadline`, `want/a-settled-want-disarms-the-timer` and
`want/a-registration-at-the-deadline-loses`.

## `context`

What a plugin sees of the core, and the only way it reaches any of it. A context is built for a plugin when the host
opens it. Once the plugin is released or quarantined its listeners still hear the events its release or quarantine
call itself queued, its wants rejected `stopped` among them, and nothing queued after them; a call it still makes
reaches the core as a late call, which the core answers.

- **Services.** A plugin gets a service registered now, wants one, watches one, lists the ids it may reach that are
  registered now, and registers its own.
  - A get, a settled want and a watch hearing a registration each answer the provider's own object (cross-cutting
    rules), looked up from the registration handle the core answered.
  - A want settles once, when the core settles it: with the provider's object, or refused with the core's refusal, its
    code, subject and fix intact. A want waits without bound unless the plugin asks for a timeout, and a constrained
    want takes its budget and its poll interval from the plugin, every one of them handed to the core unchanged. A
    want the core rejects because its plugin stopped is rejected to the plugin, which still hears it.
  - A watch hears every later registration and unregistration the core reports for the id, the object on the one and
    nothing on the other, until the plugin stops watching.
  - A registration lasts until the plugin unregisters it or stops.
- **Capacity answering is opt-in at registration.** A plugin that registers a service may hand the layer what answers
  a constrained want's questions about it: its live attributes and whether it serves a consumer under given
  constraints. Each `WEIVEL_EVENT_CAPACITY_WANTED` is put to that answerer and its answer reported with
  `weivel_report_capacity`. A registration that offered none answers at once with no live attributes, so its
  manifest's advertised attributes stand, and that it serves every consumer. An answerer that fails is reported to the
  error sink and its question left unanswered, which the core reads as not served for now.
- **Capabilities.** A plugin provides an implementation of a capability its manifest declares, lists the verified
  implementations of one, and watches them join and leave. An implementation reaches consumers only once its
  provider's activation is verified.
- **Events.** A plugin hears what the core reports about it: a service or implementation revoked, so it must stop
  using it; a refusal of reach; a constrained want that settled degraded.
- **Log.** A plugin's log is the host's to direct. The core's own log events go to the host's log at their level.
- **Manifest.** The plugin's manifest value, as the host registered it.
- **Listeners never reach a caller.** A listener that fails is reported to the error sink, and every other listener
  still hears each change in order. The failure is not a refusal and is not reported to the core.
  Witnessed by `consume/a-throwing-listener-does-not-silence-the-others`.
- It may not cache an answer the core gives, wrap a provider's object, or decide anything the core decides: reach,
  scope, settlement, revocation and order are the core's.

## `host`

Registers and loads plugins written in the language, activates and releases them, offers host services, and loads C
libraries as host services.

**Registration.** The host registers a plugin from a manifest value and an entry the caller hands it, and answers the
plugin's handle or the core's refusal. It never requires the standard: reading a manifest file, discovering plugins on
disk and reading a library declaration are the `standard` module's, which feeds this registration. A host created
without the standard runs every plugin its caller registers.

**A plugin's entry**, in abstract, offers an activate that takes the plugin's context and may finish later, and
optionally a deactivate, which may finish later too. Each language names its concrete form in its own repository and
in nowhere else.

**Host services.** The host registers a service of its own under `WEIVEL_HOST`, offered to the activation plan as
provided by the host, and its object is answered as any provider's is.

**Activation.** Given plugins and the edge source its caller chooses, the host asks the core for the plan, then:

1. starts each plugin of the plan's order in that order: opens it, builds its context and calls its activate;
2. never waits for one plugin's activate to finish before starting the next, because a plugin whose activate waits on
   a service a later plugin registers would otherwise hold back every plugin behind it for good;
3. as each activate settles, quarantines the plugin when it failed, with a reason saying so and a fix, and otherwise
   verifies it with `weivel_verify_activation`; a verification the core refuses quarantines the plugin in the same
   step, before anything else runs, with no reason of the host's, so its ledger row shows the core's refusal;
4. returns once every activate has been started, without blocking on any of them settling; each plugin's outcome,
   active, quarantined or refused, arrives later, in whatever form the language uses for a result that comes later
   (a promise, a future, a callback the caller registers, or an outcome the caller reads back). A host whose
   activation blocked until every activate settled would never return while an activate waits on a want nothing
   provides, and so could never be destroyed.

Witnessed by `lifecycle/an-awaited-want-does-not-hold-back-a-later-plugin` and
`consume/a-refused-verify-quarantines-the-plugin`.

**Release.** Releasing a plugin, in this order:

1. calls its deactivate, if it offers one, while the plugin is still whole, so the deactivate can still use what it
   holds; a deactivate that fails is reported to the error sink and the release goes on;
2. releases it in the core with `weivel_release_plugin`;
3. drains, dispatching every event that release queued to the plugin, so a want it still waited on is rejected
   `stopped` to it;
4. ends the plugin's own entries: drops its watches, listeners, registrations and answerers from the layer's tables,
   calling nothing in the core, since the release has already ended them there and a call with their handles would
   only be refused as stale;
5. forgets it with `weivel_forget_plugin`, dropping whatever the revocations that call queues name;
6. drops it from every table of the host's.

A plugin whose activate is still running when it is released is released the same way, and its activate settling
later verifies nothing, quarantines nothing and reports nothing: no activation is left settling for a plugin the host
no longer holds.

Witnessed by `lifecycle/a-release-deactivates-then-releases-then-forgets`.

**Destroy.** Destroying the host:

1. releases every plugin the host opened and has not released, one whose activate is in flight included, in the
   reverse of the order it opened them, so a consumer is released while what it consumes is still there;
2. unloads every C library it loaded; an unload that fails is reported to the error sink and the rest still unload;
3. disarms the timer and destroys the core, whatever the steps before it reported.

Destroy does not free the host object itself. A call made on the host after it is destroyed, a verify among them,
answers quietly: it answers that the host is destroyed, as a value, and neither throws nor writes to the log or the
error sink. Nothing the destroy sets off, an in-flight activate ending with it included, reaches the host's caller or
its sinks. A second destroy answers the first one's result. In a language that frees memory by hand, the host is
freed by a separate call made after destroy, and calls on it are defined only until that free; a language whose
memory is collected frees it when the last reference goes.

Witnessed by `lifecycle/destroy-releases-a-plugin-whose-activate-is-in-flight`.

**C libraries as host services.** The host loads a C library from a declaration value, read by the standard or built
by the caller, binds each declared function of the standard's bytes kind (`WEIVEL_STANDARD_FUNCTION_BYTES` in the
standard's `library.h`), and registers each declared service under `WEIVEL_HOST` as an object with one function per
declared function.

- The input crosses borrowed: the layer passes the language's bytes for the length of the call and never copies them
  where the language can lend its memory.
- The output is the library's, freed only by the library's declared release symbol, and bytes are never answered
  after their release. One of two things happens: either the layer copies the bytes once into the language's own
  memory and then releases them before the call answers, which a garbage-collected language must do; or the layer
  answers the bytes in place and defers the release until the language's owner of them drops them, calling the
  release symbol then. Never more than one copy.
- A status other than 0 answers a failure carrying the status and the body, read as UTF-8 where it is one.
- Unloading unregisters every service of the library and refuses any call made after it.
- There is no other function kind: the value 1 is retired, and a declaration naming any other kind is refused by the
  standard's reader before the layer sees it.

**It may not** read a file, a manifest format or the environment on its own account; those are the standard's
defaults, or the caller's.

## `standard` (optional)

The language's access to the standard's defaults through the same foreign function interface the core uses: reading
a JSON manifest into a manifest value, reading a library declaration, laying out and discovering plugins on disk,
identities, and the ids and definitions of the config, secrets and paths services (`services.h`), each translated
from the one C definition there.

- Every function here feeds the host: discovery answers manifest values and entries the caller hands to the host's
  registration.
- A repository may omit this module, and a host never requires it.
- It may not hold a default anywhere but in the standard's binary, nor add a format, a file name or a variable the
  standard does not own.

## `conformance`

The driver for the layer suite.

- Every layer vector of the kit commit the language pins is witnessed by a test that names its id. The kit's `layers`
  drift check, run in the language's CI, fails when a vector's id appears nowhere in the language's test sources; the
  language's own coverage test, which reads its tests' names, stays the strong check.
- The driver builds each vector's plugins as manifest values and entries and registers them through the host's public
  surface, writing no file.
- The core's and the standard's vectors are the kit runner's; the layer suite runs only the layer's.
- It may not carry a vector of its own making in place of the kit's, nor a copy of a kit vector as a fixture.

## `benchmarks`

The language's half of the platform's benchmarks, gating CI.

- Every bound is the kit's: a language that crosses a foreign function interface reads its bounds from
  `benchmarks/<language>.txt` of the kit commit it pins, and no repository sets its own.
- The same-language gate, in every language, is the identity check: the service a consumer resolves is the provider's
  own object. The timing reported beside it is judged only against the noise bounds.
- The ratio gates, the control call and the library call each over a bare foreign call through the same binding,
  apply only to a language that crosses one.
- A C layer crosses none: it gates its control call against the core's own budget, the kit's `benchmarks/budget.txt`.
- A gate run on a platform the bounds give no line fails rather than passing unbounded.
- It may not loosen a bound, skip a gate, or pass with nothing measured.

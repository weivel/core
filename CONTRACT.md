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

- Before the call, the wrapper hands the core a deadline that has fallen due: it reads its clock, and when the clock
  has reached the deadline it armed from the latest `WEIVEL_EVENT_DEADLINE` (see `timer`), it calls
  `weivel_advance(now)` through itself first, drains that as any call, and only then makes the call it was asked for.
  The advance's events are dispatched as any call's are: before the call when no dispatch runs, and at the end of the
  queue, ahead of the call's own, when one does, so the dispatcher is never re-entered. Neither the advance it makes
  for this nor an advance the layer's caller asks for is checked first, so the check never recurses. It costs one
  clock read per call while a deadline is armed and none otherwise. A layer running no timer has armed nothing and
  makes no check.
- The core's creation is drained as any call is, so the events it queues are heard only by a listener given when the
  core is created.
- The wrapper makes the call, then drains: it calls `weivel_next_event` while the core reports events waiting, taking
  the count first from the call's own `pending_events` and then from each `weivel_next_event`, and stops when the
  count is 0 or the core answers `WEIVEL_STATUS_NONE`. A call that reports none waiting costs no further crossing.
- Each event is copied into the language's own values and handed to `weivel_event_release` before the next is
  drained, whatever the copy does, so the core never meets an unreleased event. An event whose texts or arrays the
  layer cannot copy is still dispatched, because a settlement or a revocation that went unheard would never be heard
  again: every handle, kind, code and number exact, each text a fixed sentence of the layer's own, the event marked
  incomplete, and the error sink told. Only an event of which the layer cannot hold even that is dropped, reported to
  the error sink, and the rest are still drained.
- A refused call is answered to the caller with the refusal the core queued for it, read from the drained events.
  Where the core queues none, the layer answers the status alone, with the reason the core's documentation gives
  for that case: a refusal of reach already reported once for that plugin and id is a repeat and answers
  `WEIVEL_STATUS_REFUSED` with no new refusal; a registration or watch from a plugin no longer running is a late call
  and queues only a log; `weivel_handle_state` refuses a handle that names nothing with no event at all. The layer
  never invents a refusal the core did not give.
- `WEIVEL_STATUS_MALFORMED` is a defect in the layer, reported as one, only for a call the layer built whole. A call
  that carries a structure the layer's caller built and the layer handed on unchanged, a manifest value, the core's
  options or an edge source among them, is the caller's mistake: it is answered to the caller with the core's refusal
  and reported to no sink.
- A call and its drain are one step against every other call: the events drained after a call are that call's, and
  no other thread's call may land between them. A language with several threads either holds one layer lock across
  the call and its drain, or makes every call on one owning thread that others hand their calls to. No two threads
  drain at once; under the lock each caller drains its own call.
- Dispatch never runs under the layer lock: the lock covers a call and its drain and is let go before any event is
  dispatched, so a listener may call in. Exactly one thread dispatches the shared queue at a time. A call from another
  thread while a dispatch runs is outside that dispatch: it takes the lock, makes its call and drains, and its events
  join the end of the queue the active dispatcher delivers, in the order they were drained, rather than being
  dispatched by the calling thread. So a release or a destroy made then defers its later steps exactly as one made
  inside a dispatch does (see `host`).
- Nothing of the caller's is called under a lock of the layer's: not a listener, a deferred step, the clock, the
  scheduler, the error sink or the log, so each may call in.
- It may not keep a list of which calls move a deadline, interpret a handle, or call any core function outside the
  wrapper.

## `handles`

The table from the core's handles to the language's objects: plugins, service registrations, implementations of
capabilities, watches and wants, and what each plugin holds of another's.

- An entry is made once the core answers the handle and before any event naming it is dispatched, and ends when what
  the handle stands for ends: an unregistration, a withdrawn implementation, a stopped watch, a settled or withdrawn
  want, a released or forgotten plugin. Making it cannot fail once the core has answered: the layer takes the room it
  needs before the call, so the core never holds an object whose handle the table lacks.
- A holding is the object a plugin was handed for another's registration or implementation, kept by the pair of the
  plugin's handle and that handle. A `WEIVEL_EVENT_REVOKED` names the pair and drops that holding. Its `plugin` may
  name a forgotten plugin, and is then only half of a key and never used as a handle to call with.
- A handle the table does not hold answers nothing. It is never matched to a newer object, because the core never
  reissues a handle while its object lives and refuses a stale one as data.
- A plugin may be opened with the layer's own token as its payload (`weivel_open_plugin`), so the layer reaches its
  record from a plugin handle without a search.
- It may not hold an object the core has stopped naming, nor answer one plugin's object to another unless the core
  answered that handle to it.

## `timer`

Arms the core's next deadline on the language's own timer.

- The timer is armed from the latest `WEIVEL_EVENT_DEADLINE` drained, and from nothing else: `set` 1 arms it for
  `at`, replacing any deadline armed before, and `set` 0 disarms it. It is armed as the wrapper drains the event, or
  once the call's drain is done and before any of its events is dispatched, never later at dispatch, because a
  dispatch can run after a newer announcement has already been drained. Because the core keeps at most one
  announcement waiting and queues each after every other event of its call, the latest one drained is always the
  current deadline, and where several threads drain, the timer ends armed for the latest one drained, whichever
  thread drained it.
- Beside the wrapper's check (see `core`) and the time a context hands the core with a want or a capacity answer that
  needs one, the clock is read only by the timer: when its callback runs, and when it schedules a wait that ends short
  of the deadline.
- When the timer fires it hands the core the time through the wrapper with `weivel_advance(now)`. The events that
  call queues re-arm or disarm it as any others do.
- A language whose timer cannot wait as long as a deadline lies away waits as long as it can and arms again when that
  wait ends, never calling `weivel_advance` before the deadline it armed.
- The core learns a deadline has passed before any other call is made, never after it. The timer firing is one way it
  learns; the wrapper's check before each call (see `core`) is the other, and it does not wait for the language's
  scheduler: when the clock has reached the armed deadline but the timer has not run yet, because the scheduler has
  not taken its turn, the process was suspended or the deadline and other work fall due at the same moment, the
  wrapper advances the core first. So a registration at or after a want's deadline settles nothing.
- With no deadline there is no timer, so a want that asked for no timeout waits for as long as it takes.
- It may not keep a list of the calls that move a deadline, poll the core for its deadline, or advance the core on a
  time of its own reckoning.

Witnessed by `want/a-timed-want-arms-the-timer-at-its-deadline`, `want/a-settled-want-disarms-the-timer`,
`want/a-registration-at-the-deadline-loses` and `want/a-registration-past-an-unfired-deadline-loses`.

## `context`

What a plugin sees of the core, and the only way it reaches any of it. A context is built for a plugin no later than
the host opens it, and an event drained before a context was made never reaches it, since the handle it names may
then have stood for another plugin. Once the plugin is released or quarantined its listeners still hear the events
its release or quarantine call itself queued, its wants rejected `stopped` among them, and nothing queued after them,
which is why its entries end only once those events are dispatched (see the host's release). A call it still makes
reaches the core as a late call, which the core answers, and a want it makes then is still settled to it.

Once the core has forgotten the plugin, its context is detached: every call through it that would reach the core is
answered as detached, an answer of its own that is neither a success nor the core's refusal, without reaching the
core, because the core may by then have issued the plugin's handle to another plugin. Whether a call is detached is
decided under the layer's lock as the call is made, and the host detaches the context in the same hold of that lock
as its forget, so no call that passed the check reaches the core after the forget.

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
  error sink and its question left unanswered, which the core reads as not served for now. A question that arrived
  incomplete is left unanswered the same way, without being put to the answerer, because its constraints are missing
  rather than empty; the sink hears of it once, from the wrapper that marked it.
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
plugin's handle or the core's refusal. It copies the manifest value whole, keeps that copy until the host is freed,
and registers and answers the copy, so its caller may free or change its own once registration returns. It never
requires the standard: reading a manifest file, discovering plugins on disk and reading a library declaration are the
`standard` module's, which feeds this registration. A host created without the standard runs every plugin its caller
registers.

**A plugin's entry**, in abstract, offers an activate that takes the plugin's context and may finish later, and
optionally a deactivate, which may finish later too. Each language names its concrete form in its own repository and
in nowhere else.

**Host services.** The host registers a service of its own under `WEIVEL_HOST`, offered to the activation plan as
provided by the host, and its object is answered as any provider's is.

**Activation.** Given plugins and the edge source its caller chooses, the host asks the core for the plan, then:

1. starts each plugin of the plan's order in that order: builds its context no later than it opens it, opens it and
   calls its activate. Plan order binds the call to each entry's activate and nothing the entry does after that call
   returns: an entry that loads its plugin's code later may run that code in whatever order the loads finish;
2. never waits for one plugin's activate to finish before starting the next, because a plugin whose activate waits on
   a service a later plugin registers would otherwise hold back every plugin behind it for good;
3. as each activate settles, quarantines the plugin when it failed, with a reason saying so and a fix, and otherwise
   verifies it with `weivel_verify_activation`; a verification the core refuses quarantines the plugin before any
   event of the verification is dispatched and before the host starts the next plugin, with no reason of the host's,
   so its ledger row shows the core's refusal;
4. returns once every activate has been started, on the activating thread and each exactly once, whether or not
   another thread is dispatching meanwhile, without blocking on any of them settling, and returns the plan, its
   order the order the activates were started in, so the caller can read it at once; each plugin's outcome arrives
   later, in whatever form the language uses for a result that comes later (a promise, a future, a callback the
   caller registers, or an outcome the caller reads back). A host whose activation blocked until every activate
   settled would never return while an activate waits on a want nothing provides, and so could never be destroyed.

An outcome is one of five: `active`, `quarantined`, `refused` when the core refused to open the plugin, `released`
for a plugin released while its activate was in flight, answered when that release begins, so every outcome arrives
even for an activate that never settles, or for one the destroy kept from its activate (see Destroy), and `failed` for
a plugin whose open failed for a reason other than the core's refusal. Such an open is a defect of the layer: it is
reported to the error sink and answered as that plugin's `failed` outcome with no refusal. When the open reached the
core at all, so that the core holds the plugin open, the layer releases and forgets it in the core at once, by the
release steps that still apply (its activate was never called, so no deactivate is owed), and answers the outcome
once they have run, so no plugin stays open in the core until destroy without an outcome. The activation goes on to
the next plugin, so the caller still receives the plan and every other outcome and no started plugin is left running
without one.

A verification the caller asks for is carried out as step 3 carries one out. One of a plugin the host never opened is
answered as the core answers it and quarantines nothing, since the host holds nothing of that plugin open.

Witnessed by `lifecycle/an-awaited-want-does-not-hold-back-a-later-plugin` and
`consume/a-refused-verify-quarantines-the-plugin`.

**Release.** Releasing a plugin, in this order:

1. calls its deactivate, if it offers one, while the plugin is still whole, so the deactivate can still use what it
   holds; a deactivate that fails is reported to the error sink and the release goes on;
2. releases it in the core with `weivel_release_plugin`;
3. drains, and lets every event that release queued be dispatched to the plugin, so a want it still waited on is
   rejected `stopped` to it;
4. once those events have been dispatched, and not before, ends the plugin's own entries: drops its watches,
   listeners, registrations and answerers from the layer's tables, calling nothing in the core, since the release has
   already ended them there and a call with their handles would only be refused as stale;
5. forgets it with `weivel_forget_plugin`, dropping whatever the revocations that call queues name;
6. drops it from every table of the host's.

A release made inside a dispatch cannot dispatch its own events in step 3, because the dispatcher is never re-entered
and those events join the end of the queue. So steps 4 to 6 are never done eagerly there: the layer puts them on the
dispatch queue as a deferred step behind the events the release queued, and they run once those events have been
dispatched. The same holds for a quarantine (its entries end after the events its quarantine call queued) and for each
release a destroy makes. `lifecycle/a-release-deactivates-then-releases-then-forgets` exercises only a release made
at the top level, outside any dispatch, and does not tell entries ended and a forget made before that release's
events are dispatched from ones made after; both are held by review and the language's own tests.

A release counts as made inside a dispatch when its core release, step 2, runs inside one. A deactivate that finishes
later moves step 2 and everything after it to when it finishes, so whether they are deferred is decided then, not when
the release was asked for. A release waits for the deactivate however long it takes, and so does a destroy: the layer
sets no timeout of its own, and a caller that wants a bound sets its own around the release or destroy it asks for.

A plugin whose activate is still running when it is released is released the same way, its outcome is `released`, and
its activate settling later verifies nothing, quarantines nothing and reports nothing further: no activation is left
settling for a plugin the host no longer holds.

A plugin's own code may still run after its release and after the destroy has finished, so a language that loads a
plugin's code from a library it opened closes that library only when the host is freed.

Witnessed by `lifecycle/a-release-deactivates-then-releases-then-forgets`.

**Destroy.** Destroying the host:

1. releases every plugin the host opened and has not released, one whose activate is in flight and one quarantined
   included, in the reverse of the order it opened them, so a consumer is released while what it consumes is still
   there. Each release calls the plugin's deactivate when its activate was called, a quarantined plugin's among them.
   A plugin whose open the core refused was never opened and is never released, and one whose open failed was
   released when it failed, if the core held it open at all. The destroy waits for every open and every settling of
   an activate begun while the host was running. A plugin opened as the destroy began is released at once without
   its activate being called, and a plugin of a plan not yet started is not opened; each is answered `released`;
2. dispatches every event those releases queued and runs every deferred step they left to completion, each release's
   forget included, so no listener, deferred step or core call is left to run once the core is gone;
3. unloads every C library it loaded; an unload that fails is reported to the error sink and the rest still unload;
4. disarms the timer and destroys the core, whatever the steps before it reported.

A destroy called inside a dispatch cannot dispatch the events ahead of it, because the dispatcher is never
re-entered. So it is deferred the same way a release is: the layer puts it on the dispatch queue as a deferred step
behind every event and deferred step already queued, and it runs, from step 1, once they have run. Running there, it
is still inside the dispatch, so its steps 3 and 4 wait in turn as a further deferred step behind the events and
deferred steps its own releases queued, which is how step 2 holds.

Destroy does not free the host object itself. A destroy's own releases tell their outcomes and report a failing
deactivate as any release does. What is quiet is every other call made on the host once its destroy has been called,
a verify among them, and an activate settling after it: such a call answers that the host is destroyed, as a value,
a second destroy answering the first one's result instead, and neither throws nor writes to the log or the error
sink, and such an activate reaches nothing. Once the host is destroyed nothing more is reported. In a language that
frees memory by hand, the host is freed by a separate call made after destroy, and calls on it are defined only until
that free; a language whose memory is collected frees it when the last reference goes.

Witnessed by `lifecycle/destroy-releases-a-plugin-whose-activate-is-in-flight`.

**C libraries as host services.** The host loads a C library from a declaration value, read by the standard or built
by the caller, and the library's path, which its caller hands it beside the declaration since a declaration names
none. It binds each declared function of the standard's bytes kind (`WEIVEL_STANDARD_FUNCTION_BYTES` in the
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
- Unloading closes the library to calls at once: it unregisters every service of the library, and a call made after
  it through an object a consumer still holds is refused. The library's binary is closed only once no call is
  running in it and no bytes it answered are held, whichever ends last.
- A load is refused as the caller's mistake, answered to the caller and reported to no sink, when an id it declares
  appears twice in the declaration or is already registered under the host, by another live library, a plugin or the
  host itself, because a registration under `WEIVEL_HOST` replaces one. The host's own registration of an id a live
  library declares is refused the same way.
- There is no other function kind: the value 1 is retired, and a declaration naming any other kind is refused by the
  standard's reader before the layer sees it. A declaration the caller builds itself with a kind other than bytes is
  refused by the host as the caller's mistake, answered to the caller and not reported as a defect of the layer.
- The host may use the standard's published declaration types, the layout of a declaration and its kind constants,
  but it never loads or calls the standard's binary.

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

- Every bound is the kit's: a language reads its bounds from `benchmarks/<language>.txt` of the kit commit it pins,
  and no repository sets its own.
- The same-language gate, in every language, is the identity check: the service a consumer resolves is the provider's
  own object. The timing reported beside it is judged only against the noise bounds.
- The ratio gates hold the control call through the wrapper, and a C library's function called through the object the
  host registers for it, each over the same call made without the layer: through the bare binding in a language that
  crosses a foreign function interface, and in C through the bare table of the core's functions and the platform's
  loader.
- A C layer, which crosses no foreign function interface, also gates its control call through the wrapper against
  the core's own budget, the kit's `benchmarks/budget.txt`.
- A gate run on a platform the bounds give no line fails rather than passing unbounded.
- It may not loosen a bound, skip a gate, or pass with nothing measured.

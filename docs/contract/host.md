# `host`

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
in nowhere else. An activate may run as the dispatcher, so it must not block on an event another thread causes: one
that must wait for such an event answers a result that comes later. A layer loads a plugin's code by a name its caller
gives, and a language's module convention is not a default the layer may assume.

**The ledger and the timer.** The host answers its caller the core's ledger, every plugin's row as the core keeps it,
and lets its caller see each move of its timer, the deadline it arms or that it disarms: the ledger is what the layer
vectors read a plugin's state and its quarantine reason from, and the timer's moves are what the `want` vectors record.
It hands its caller nothing of the wrapper's dispatch, clock or deadline hooks, so nothing but the layer drains the
core or moves its timer. A language with no way to hide a type its own modules share keeps that type public and
documents it as unsafe for a caller, as Java 8 must, having no module system.

**Host services.** The host registers a service of its own under `WEIVEL_HOST`, offered to the activation plan as
provided by the host, and its object is answered as any provider's is.

**Activation.** Given plugins and the edge source its caller chooses, the host asks the core for
[the plan](../plan.md), then:

1. starts each plugin of the plan's order in that order: builds its context no later than it opens it, opens it and
   calls its activate. Plan order binds the call to each entry's activate and nothing the entry does after that call
   returns: an entry that loads its plugin's code later may run that code in whatever order the loads finish. Where
   no dispatch runs, the activating thread starts the plan as the dispatcher, so what an activate's own calls queue is
   dispatched once every activate of the plan has been called; where one runs, the plan is started as a call made
   while that dispatch runs, and those events join the end of its queue. Either way an activate that settles, as
   the language can observe it, before it returns is verified, and quarantined if the core refuses, before the
   next plugin is started;
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

A plan the core refuses is answered to the caller as that refusal, as a value, and never thrown. A plan with cycles is
not a refusal: it is answered to the caller with its cycles, and only a malformed call is refused.

An outcome is one of five: `active`, `quarantined`, `refused` when the core refused to open the plugin, `released`
for a plugin released while its activate was in flight, answered when that release begins, so every outcome arrives
even for an activate that never settles, or for one the destroy kept from its activate (see Destroy), and `failed` for
a plugin whose open failed for a reason other than the core's refusal. Such an open is a defect of the layer: it is
reported to the error sink and answered as that plugin's `failed` outcome with no refusal. When the open reached the
core at all, so that the core holds the plugin open, the layer releases and forgets it in the core at once, by the
release steps that still apply (its activate was never called, so no deactivate is owed), and answers the outcome
once they have run, so no plugin stays open in the core until destroy without an outcome. The activation goes on to
the next plugin, so the caller still receives the plan and every other outcome and no started plugin is left running
without one. An outcome carries the refusal that decided it: a `refused` outcome the core's refusal of the open, and a
`quarantined` outcome the core's refusal of the verification when that refusal is what quarantined the plugin,
and a quarantine for a reason of the host's carries none.

A plugin is opened once while it stays open. An open, or an activation, of a plugin the host holds open, or whose
activate is in flight, is refused by the core before the host builds anything for it, its outcome `refused`, and the
plugin's context is unchanged. A quarantined plugin may be opened again, and keeps its context.

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

A release of a plugin the host has not opened, registered only or already forgotten, answers that there was nothing to
release and does nothing.

Witnessed by `lifecycle/a-release-deactivates-then-releases-then-forgets`.

**Destroy.** Destroying the host:

1. releases every plugin the host opened and has not released, one whose activate is in flight and one quarantined
   included, in the reverse of the order it opened them, so a consumer is released while what it consumes is still
   there. Each release calls the plugin's deactivate when its activate was called, a quarantined plugin's among them.
   A plugin whose open the core refused was never opened and is never released, and one whose open failed was
   released when it failed, if the core held it open at all. The destroy waits for every open begun while the host
   was running, and for every settling of an activate that was already under way as it began; it never waits for an
   activate to settle, and an activate that has not settled is released as in flight. A plugin opened as the destroy
   began is released at once without its activate being called, and a plugin of a plan not yet started is not opened;
   each is answered `released`;
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
none. A declared function is of one of the two kinds the standard's `library.h` names, bytes
(`WEIVEL_STANDARD_FUNCTION_BYTES`) or definition (`WEIVEL_STANDARD_FUNCTION_DEFINITION`), and the host registers each
declared service under `WEIVEL_HOST`: a service of bytes functions as an object with one function per declared
function, and a service that declares a definition as that definition.

- A bytes function's input crosses for the length of the call only: memory the language can lend without holding off
  its collector is lent, and memory the collector may move is copied once.
- A bytes function's output is the library's, freed only by the library's declared release symbol, and bytes are
  never answered after their release. One of two things happens: either the layer copies the bytes once into the
  language's own memory and then releases them before the call answers, which a garbage-collected language must do;
  or the layer answers the bytes in place and defers the release until the language's owner of them drops them,
  calling the release symbol then. Never more than one copy.
- A bytes function's status other than 0 answers a failure carrying the status and the body as bytes, the body read
  as UTF-8 text only where it is well-formed UTF-8.
- A definition is a symbol `const void *f(void)` answering a static struct of function pointers that begins with its
  `uint32_t size` and lives until the library is unloaded. Its layout, its operations and their memory rules are the
  library's, published in its own header, and the host never reads past `size`. A service that declares a definition
  declares it as its one function, with no context facts, and a library whose services are all definitions needs no
  release symbol, since each definition carries its own release.
- At load the host resolves a definition's symbol, calls it once and registers the pointer it answered as the
  service's object, with `weivel_register_service_awaiting_releases`: in C the pointer itself, and in any other
  language an object carrying that native address. It adds no wrapper, count or copy, so a consumer's call through a
  definition is a plain native call. A definition that answers NULL, or a struct whose `size` is below
  `2 * sizeof(void *)` (the `size` and one pointer under the platform's alignment: 8 on 32-bit, 16 on 64-bit),
  refuses the load as the caller's mistake, undone as a load that fails partway.
- Unloading unregisters every service of the library at once. A call made after it through a bytes object a consumer
  still holds is refused. A definition is called directly, so no call to it can be refused: its unregistration
  revokes every holding of it, each holder told by a `WEIVEL_EVENT_REVOKED`, and the core keeps each holding until it
  is released. A holder stops calling on that revocation, releases every answer, refusal and handle the definition
  gave it, and then releases the holding itself with `weivel_release_holding`; a holder the host releases,
  quarantines or forgets has its holding ended by the core.
- A call through a definition made after its holder's holding ended, by `weivel_release_holding` or because the host
  released, quarantined or forgot the holder, is the holder's defect and is undefined, as any use of memory after its
  free is: nothing then keeps the binary open for it. A host that hands a definition's address to a caller that holds
  no registration of it, as a loader that reads it to bind, says where it hands it that the address dies with the
  binary, since that caller's use keeps nothing open.
- The library's binary is closed once the core has queued a `WEIVEL_EVENT_HOLDINGS_RELEASED` for every one of its
  definition services, no call into a bytes function of it is running and no bytes it answered are held, whichever
  ends last; or at the host's free, since the destroy ends every registration without that event. A language whose
  memory is collected, and so has no free, closes it once its destroy has destroyed the core. Any address a definition
  answered is invalid from then on.
- A load that fails partway, as when the core refuses one of its registrations, is undone as an unload: what it
  registered is unregistered, and its binary is closed once nothing of it can run.
- A load is refused as the caller's mistake, answered to the caller and reported to no sink, when an id it declares
  appears twice in the declaration or is already registered under the host, by another live library, a plugin or the
  host itself, because a registration under `WEIVEL_HOST` replaces one. The host's own registration of an id a live
  library declares is refused the same way.
- There are two function kinds and no third: the value 1 is retired, and a declaration naming any other kind, or a
  definition beside another function of its service, is refused by the standard's reader before the layer sees it.
  A definition reads no context facts, since its struct carries its own context, so the reader refuses a `context`
  on one too. A declaration the caller builds itself that names another kind, sets a definition beside another
  function, or gives a definition context facts other than none is refused by the host as the caller's mistake,
  answered to the caller and not reported as a defect of the layer.
- The host may use the standard's published declaration types, the layout of a declaration and its kind constants,
  but it never loads or calls the standard's binary.

**It may not** read a file, a manifest format or the environment on its own account; those are the standard's
defaults, or the caller's. A default path to a binary inside the language's own package, as a TypeScript package
carries the core's and the standard's, is permitted, since it forces nothing on a caller who passes a path.

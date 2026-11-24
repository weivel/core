# `context`

What a plugin sees of the core, and the only way it reaches any of it. A context is built for a plugin no later than
the host first opens it, and the plugin keeps that one context until it is forgotten, a quarantined plugin opened
again included. An event drained before a context was made never reaches it, since the handle it names may then have
stood for another plugin. Once the plugin is released or quarantined its listeners still hear the events its release
or quarantine call itself queued, its wants rejected `stopped` among them, and nothing queued after them, which is why
its entries end only once those events are dispatched (see [the host's release](host.md)). A call it still makes
reaches the core as a late call, which the core answers, and a want it makes then is still settled to it.

Once the core has forgotten the plugin, its context is detached: every call through it that would reach the core is
answered as detached, an answer of its own that is neither a success nor the core's refusal, without reaching the
core, because the core may by then have issued the plugin's handle to another plugin. Whether a call is detached is
decided under the layer's lock as the call is made, and the host detaches the context in the same hold of that lock
as its forget, so no call that passed the check reaches the core after the forget.

- **Services.** A plugin gets a service registered now, wants one, watches one, lists the ids it may reach that are
  registered now, and registers its own.
  - A get, a settled want and a watch hearing a registration each answer the provider's own object
    ([cross-cutting rules](cross-cutting.md)), looked up from the registration handle the core answered.
  - A want settles once, when the core settles it: with the provider's object, or refused with the core's refusal, its
    code, subject and fix intact. A want waits without bound unless the plugin asks for a timeout, and a constrained
    want takes its budget and its poll interval from the plugin, every one of them handed to the core unchanged. A
    want the core rejects because its plugin stopped is rejected to the plugin, which still hears it. A plugin may
    withdraw a want that has not settled, and its result is then never delivered.
  - A watch hears every later registration and unregistration the core reports for the id, the object on the one and
    nothing on the other, until the plugin stops watching.
  - A registration lasts until the plugin unregisters it or stops.
  - A get, a list of the ids, a watch and a listing of a capability's implementations that the core refuses each
    answer that refusal, never an empty answer or a watch that hears nothing. A get that finds nothing registered
    answers absence, which is not a refusal.
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
  using it; a refusal of reach; a constrained want that settled degraded; and, for a service it registered with
  `weivel_register_service_awaiting_releases`, its `WEIVEL_EVENT_HOLDINGS_RELEASED`, which the layer drains as any
  event and hands to the plugin that owns the registration, telling it nothing holds what it registered any more, so
  whatever stood behind it may be freed. An owner hears it as it hears any event about it: one that its own release
  or quarantine call queued, as when nothing held the registration then, still reaches its listeners; one queued
  after those events, or after it was forgotten, reaches no one, and the layer drops it.
- **Log.** A plugin's log is the host's to direct. The core's own log events go to the host's log at their level.
- **Manifest.** The plugin's manifest value, as the host registered it.
- **Listeners never reach a caller.** A listener that fails is reported to the error sink, and every other listener
  still hears each change in order. The failure is not a refusal and is not reported to the core.
  Witnessed by `consume/a-throwing-listener-does-not-silence-the-others`.
- It may not cache an answer the core gives, wrap a provider's object, or decide anything the core decides: reach,
  scope, settlement, revocation and order are the core's.

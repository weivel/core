# `core`

Loads the core's binary and owns the one wrapper every call goes through.

- Before the call, the wrapper hands the core a deadline that has fallen due: it reads its clock, and when the clock
  has reached the deadline it armed from the latest `WEIVEL_EVENT_DEADLINE` (see [`timer`](timer.md)), it calls
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
  incomplete, and the error sink told. An event of a kind the layer does not know is released and ignored without a
  report, as the core's header asks; only an event the layer cannot hold even that much of is dropped, reported to
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
- An id is the caller's value. A layer may refuse a malformed one before it calls the core, or hand it on and answer
  the core's refusal; either way it is the caller's mistake, answered to the caller and reported to no sink.
- A call and its drain are one step against every other call: the events drained after a call are that call's, and
  no other thread's call may land between them. A language with several threads either holds one layer lock across
  the call and its drain, or makes every call on one owning thread that others hand their calls to. No two threads
  drain at once; under the lock each caller drains its own call.
- Dispatch never runs under the layer lock: the lock covers a call and its drain and is let go before any event is
  dispatched, so a listener may call in. Exactly one thread dispatches the shared queue at a time. A call from another
  thread while a dispatch runs is outside that dispatch: it takes the lock, makes its call and drains, and its events
  join the end of the queue the active dispatcher delivers, in the order they were drained, rather than being
  dispatched by the calling thread. So a release or a destroy made then defers its later steps exactly as one made
  inside a dispatch does (see [`host`](host.md)).
- Nothing of the caller's is called under a lock of the layer's: not a listener, a deferred step, the clock, the
  scheduler, the error sink or the log, so each may call in.
- It may not keep a list of which calls move a deadline, interpret a handle, or call any core function outside the
  wrapper.

# `timer`

Arms the core's next deadline on the language's own timer.

- The timer is armed from the latest `WEIVEL_EVENT_DEADLINE` drained, and from nothing else: `set` 1 arms it for
  `at`, replacing any deadline armed before, and `set` 0 disarms it. It is armed as the wrapper drains the event, or
  once the call's drain is done and before any of its events is dispatched, never later at dispatch, because a
  dispatch can run after a newer announcement has already been drained. Because the core keeps at most one
  announcement waiting and queues each after every other event of its call, the latest one drained is always the
  current deadline, and where several threads drain, the timer ends armed for the latest one drained, whichever
  thread drained it.
- Beside the wrapper's check (see [`core`](core.md)) and the time a context hands the core with a want or a capacity
  answer that needs one, the clock is read only by the timer: when its callback runs, and when it schedules a wait that
  ends short of the deadline. A language whose scheduler takes a delay rather than a time also reads it each time it
  schedules, in its scheduler or its timer, to turn the deadline into that delay.
- When the timer fires it hands the core the time through the wrapper with `weivel_advance(now)`. The events that
  call queues re-arm or disarm it as any others do.
- A language whose timer cannot wait as long as a deadline lies away waits as long as it can and arms again when that
  wait ends, never calling `weivel_advance` before the deadline it armed.
- The core learns a deadline has passed before any other call is made, never after it. The timer firing is one way it
  learns; the wrapper's check before each call (see [`core`](core.md)) is the other, and it does not wait for the
  language's scheduler: when the clock has reached the armed deadline but the timer has not run yet, because the
  scheduler has not taken its turn, the process was suspended or the deadline and other work fall due at the same
  moment, the wrapper advances the core first. So a registration at or after a want's deadline settles nothing.
- With no deadline there is no timer, so a want that asked for no timeout waits for as long as it takes.
- A host its caller gives no scheduler either arms the language's own timer or arms none, and a host that arms none
  answers a want that needs the time as the caller's mistake. Each language states which in its own `CONTENT.md`.
- It may not keep a list of the calls that move a deadline, poll the core for its deadline, or advance the core on a
  time of its own reckoning.

Witnessed by `want/a-timed-want-arms-the-timer-at-its-deadline`, `want/a-settled-want-disarms-the-timer`,
`want/a-registration-at-the-deadline-loses` and `want/a-registration-past-an-unfired-deadline-loses`.

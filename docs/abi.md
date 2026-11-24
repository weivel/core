# The conventions of the ABI

What every call into the core keeps, whichever call it is: who owns what crosses the boundary, which threads may call,
how changes and time reach the caller, and how a struct grows. The header states each call's own rules beside its
declaration.

## Ownership

Every string and list handed in is copied before the call returns, so the caller may free its own at once. Every
string or list handed out belongs to the event or answer that carries it, and stays valid until that event or answer
is passed to its release call, whatever else happens meanwhile, the destruction of the core included. The core keeps
no pointer into anything it handed out, and the caller keeps no pointer into the core.

## Threads

Every call that takes a core may be made from any thread. One lock inside each core serialises the calls, and it is
never held while anything outside the core runs, which is why it cannot deadlock: the core never calls out. Two cores
share nothing. Creating and destroying a core are the only calls that must not overlap another call on the same core.
One thread drains the queue, since the events come out in the order they happened, the deadline announcement aside,
and two threads draining at once would split that order between them.

## Events and time

Every change the core makes lands in one queue, in the order it happened: a want settled, a watch heard, a service or
capability revoked, a refusal reported, a capacity question asked, and every diagnostic as a log event with its
level. Each call reports how many events wait, and the caller drains them after the call returns, outside the core.
Every event the core owes is reserved when the obligation arises: a want's settlement when the want is made, a
holding's revocation when the holding is recorded, a watch's notification before the change that causes it. A call
that cannot reserve what it will owe answers out of memory and changes nothing, so a want never vanishes unsettled
and a holder never misses a revocation. A log is the one event the core may drop when memory runs out.

Time is the caller's. Every timestamp is a count of milliseconds on a monotonic scale the caller chooses. The core
announces every change to its next deadline as an event, at most one waiting at a time, in room kept for it from the
core's creation. A newer announcement takes out one not yet drained and is queued after everything else its call
queued, so it is the one event that is not always delivered and does not keep its place in the order; what is
drained is always the current deadline. The caller arms its own timer from that event alone and tells the core the
time when the timer fires. With no deadline there is no timer, so a wait without a deadline waits for as long as it
takes.

## Sizes

Every structure the caller hands in or gets back begins with its own size, set to the size the caller was compiled
against. Fields are only ever appended. The core reads and writes only what the caller's size covers, and a field
beyond it, or left zero, takes its default, so a caller built against an older header keeps working and one built
against a newer header is read as far as this core knows. A list of structures is read at the stride its first
element declares.

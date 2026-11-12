## What this is

The weivel plugin engine, written in C. It decides: whether a plugin is compatible with a host, the order plugins
activate in, which service or capability a plugin may reach and who provides it, when a wait settles or is refused,
what is revoked when a plugin stops, and what the ledger records about every plugin. A language implementation wraps
it, and every language sees the same decisions because they are made here once.

## What it is not

It decides and does nothing else. It never calls out: no callback, no listener, no function pointer is ever invoked
by it. It owns no clock, opens no file, reads no environment variable and parses no format, so a manifest reaches it
as a structure the caller builds, never as text. It holds no plugin object, service implementation or listener:
each is an integer handle the caller maps to its own object, and a handle whose object is gone is refused as data,
never mistaken for a newer one. Loading plugins from disk, reading manifests, calling services and logging are the
caller's work.

## Ownership

Every string and list handed in is copied before the call returns, so the caller may free its own at once. Every
string or list handed out belongs to the event or answer that carries it, and stays valid until that event or answer
is passed to its release call, whatever else happens meanwhile, the destruction of the core included. The core keeps
no pointer into anything it handed out, and the caller keeps no pointer into the core.

## Threads

Every call that takes a core may be made from any thread. One lock inside each core serialises the calls, and it is
never held while anything outside the core runs, which is why it cannot deadlock: the core never calls out. Two cores
share nothing. Creating and destroying a core are the only calls that must not overlap another call on the same core.
One thread drains the queue, since the events come out in the order they happened and two threads draining at once
would split that order between them.

## Events and time

Every change the core makes lands in one queue, in the order it happened: a want settled, a watch heard, a service or
capability revoked, a refusal reported, a capacity question asked, and every diagnostic as a log event with its
level. Each call reports how many events wait, and the caller drains them after the call returns, outside the core.
Every event the core owes is reserved when the obligation arises: a want's settlement when the want is made, a
holding's revocation when the holding is recorded, a watch's notification before the change that causes it. A call
that cannot reserve what it will owe answers out of memory and changes nothing, so a want never vanishes unsettled
and a holder never misses a revocation. A log is the one event the core may drop when memory runs out.

Time is the caller's. Every timestamp is a count of milliseconds on a monotonic scale the caller chooses. The core
answers its next deadline; the caller arms its own timer for it and tells the core the time when the timer fires.
With no deadline there is no timer, so a wait without a deadline waits for as long as it takes.

## Sizes

Every structure the caller hands in or gets back begins with its own size, set to the size the caller was compiled
against. Fields are only ever appended. The core reads and writes only what the caller's size covers, and a field
beyond it, or left zero, takes its default, so a caller built against an older header keeps working and one built
against a newer header is read as far as this core knows. A list of structures is read at the stride its first
element declares.

## The ledger

The ledger answers every plugin registered and not forgotten: its handle, import name and identity, where it is in
its life, what its manifest declares, every relationship its current or last activation made (the services and
capabilities it provided and consumed), and, for a quarantined plugin, why: the reason the caller gave when it
quarantined it, or else the refusal verification answered its activation. It also answers every refusal of reach
reported for those plugins, each with its code, in the order they were reported. A forgotten plugin leaves no trace
in it.

## Still to come

The contract a language implementation follows, its modules, how it drains the queue, maps handles to objects and
arms the timer, will be written beside this description in the steps that build the first implementations.

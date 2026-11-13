#ifndef WEIVEL_CORE_H
#define WEIVEL_CORE_H

/**
 * @file
 * The weivel core: the decision engine of the weivel plugin platform, behind a C ABI.
 *
 * Conventions every declaration here follows:
 *
 * - Size. Every public struct a caller hands in or gets back begins with `uint32_t size`, set by the caller to
 *   `sizeof` the struct as the caller was compiled (WEIVEL_INIT does it). Fields are only ever appended. The core
 *   reads and writes only the bytes the caller's `size` covers; a field beyond it takes its default, and a zero
 *   field means its default too, so a zero-initialised struct asks for every default. A size smaller than the
 *   struct's first published shape is a malformed call. A struct nested inside another (an event payload) is
 *   covered by the outer struct's size and carries none of its own.
 * - Status. Every call returns a weivel_status. A malformed call (a null core, a null required pointer, a size too
 *   small) answers WEIVEL_STATUS_MALFORMED and never crashes or aborts; when it has a core, it also queues a
 *   WEIVEL_EVENT_REFUSAL saying what was wrong. A call whose refusal, of either kind, cannot be queued answers
 *   WEIVEL_STATUS_OUT_OF_MEMORY instead, so a refusal is never answered without its event.
 * - Handles. Anything the core tracks is a weivel_handle: 32 bits of slot and 32 bits of generation, never 0, valid
 *   only for the core that issued it. A handle whose object is gone is refused, never mistaken for a newer one.
 * - Events. The core never calls out. Every change lands in a queue the caller drains with weivel_next_event after
 *   the call returns; every call that can queue an event reports how many wait. An event the core owes is reserved
 *   the moment the obligation arises: a want's settlement when the want is made, a holding's revocation when the
 *   holding is recorded, and a watch's notification before the change that causes it. A call that cannot reserve
 *   everything it will owe answers WEIVEL_STATUS_OUT_OF_MEMORY and changes nothing, and a reserved event is always
 *   delivered, so no want settles unheard and no holder misses a revocation. The core always owes its caller its
 *   current deadline, so that announcement's room is reserved when the core is created and is never given back;
 *   the one announcement never delivered is one a newer announcement supersedes before it is drained. A log is the
 *   one event the core may drop when memory runs out; a rejected want whose refusal texts cannot be copied carries
 *   a fixed sentence in their place, its code always exact.
 * - Answers. An answer the core fills into a caller's struct is size-prefixed and freed by its own release call.
 *   Its arrays are of one of two kinds, and every answer says which. An element that may grow begins with its own
 *   `uint32_t size`, the core's, and the caller steps through the array by the first element's `size`, never by
 *   its own `sizeof`, reading only the fields both know: the capability list and the ledger's rows and refusals.
 *   An element that never grows carries no size, is indexed at the caller's own stride, and anything a later
 *   version adds arrives as a new array appended to the answer: the activation plan's edges, cycles and refusals,
 *   each already the whole of what it is (two handles, a list of handles, a handle and its refusal).
 * - Strings. Strings handed in are copied before the call returns. Strings handed out are UTF-8, NUL-terminated,
 *   and belong to the struct that carries them until its release call.
 * - Time. The core owns no clock. Every timestamp is a caller-supplied int64_t count of milliseconds on a
 *   monotonic scale the caller chooses. The core answers its next deadline, announces every change to it as a
 *   WEIVEL_EVENT_DEADLINE, and acts on time only in weivel_advance.
 * - Threads. Every call taking a core may be made from any thread; one lock inside the core serialises them and
 *   is never held while anything outside the core runs. One thread drains the queue: events come out in the order
 *   they happened, but for a deadline announcement, which a newer one takes out and follows every other event its
 *   call queued, and two threads draining at once would split that order between them.
 * - Enums. Enumerations travel in struct fields as uint32_t, so their width never depends on the compiler.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(WEIVEL_BUILDING_SHARED)
#define WEIVEL_API __declspec(dllexport)
#elif defined(WEIVEL_USING_SHARED)
#define WEIVEL_API __declspec(dllimport)
#else
#define WEIVEL_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define WEIVEL_API __attribute__((visibility("default")))
#else
#define WEIVEL_API
#endif

/** Initialises a size-prefixed struct of `type`, every other field zero and so defaulted. */
#define WEIVEL_INIT(type) { .size = (uint32_t)sizeof(type) }

/** What every call answers. */
typedef enum weivel_status {
    /** The call did what it was asked. */
    WEIVEL_STATUS_OK = 0,
    /** There was nothing to answer: no event waits, or no deadline is set. Not a failure. */
    WEIVEL_STATUS_NONE = 1,
    /** The call itself was wrong (a null core, a null required pointer, a size too small); nothing changed. */
    WEIVEL_STATUS_MALFORMED = 2,
    /**
     * The core declined the request. An event says why: a refusal, or a log for a late call from a plugin that is
     * no longer running. A refusal of reach is reported once per plugin and id, so a repeat answers REFUSED alone.
     */
    WEIVEL_STATUS_REFUSED = 3,
    /** Memory ran out; nothing changed. */
    WEIVEL_STATUS_OUT_OF_MEMORY = 4
} weivel_status;

/**
 * An integer standing for something the core tracks. 0 is never a valid handle. A handle is meaningful only to the
 * core that issued it and only while its object lives.
 */
typedef uint64_t weivel_handle;

/** One core instance. Opaque; created by weivel_core_create and owned by the caller until weivel_core_destroy. */
typedef struct weivel_core weivel_core;

/**
 * Why the core refused something. Each code says what its refusal's `subject` names, the text a struct carrying the
 * refusal answers beside it; a code that names nothing leaves it NULL.
 */
typedef enum weivel_refusal_code {
    WEIVEL_REFUSAL_NONE = 0,
    /** A call was malformed; detail names the call and what was wrong with it. Names nothing. */
    WEIVEL_REFUSAL_MALFORMED_CALL = 1,
    /** A handle named something that no longer exists, or never did. Names nothing. */
    WEIVEL_REFUSAL_STALE_HANDLE = 2,
    /**
     * A manifest's own shape is wrong (no import name, a NULL in a list, an id listed twice); detail says where.
     * Names the manifest's import name, or nothing when it has none.
     */
    WEIVEL_REFUSAL_MALFORMED_MANIFEST = 3,
    /**
     * This host cannot load the manifest's plugin; the answer's axis says which check refused it. Names its import
     * name.
     */
    WEIVEL_REFUSAL_INCOMPATIBLE = 4,
    /** A plugin with this import name is already registered on this core; detail names it. Names the import name. */
    WEIVEL_REFUSAL_NAME_TAKEN = 5,
    /**
     * A plugin needs something mandatory that nothing in its activation set provides; detail names the need. Names
     * the needed id.
     */
    WEIVEL_REFUSAL_UNMET_NEED = 6,
    /**
     * A plugin was listed more than once in one activation set; its later listings are refused. Names its import
     * name.
     */
    WEIVEL_REFUSAL_REPEATED_PLUGIN = 7,
    /**
     * A plugin's lifecycle does not allow the call: it was never opened, is open already, was released, or is not.
     * Names its import name.
     */
    WEIVEL_REFUSAL_PLUGIN_STATE = 8,
    /**
     * Another registered plugin that is not released already holds the identity a manifest names. Names the
     * identity.
     */
    WEIVEL_REFUSAL_IDENTITY_TAKEN = 9,
    /** A plugin registered a service id outside its namespace that is not well known. Names the service id. */
    WEIVEL_REFUSAL_OUTSIDE_NAMESPACE = 10,
    /** A plugin registered a service id another owner already holds. Names the service id. */
    WEIVEL_REFUSAL_SERVICE_TAKEN = 11,
    /** A plugin reached for a service it neither consumes nor provides in its manifest. Names the id reached for. */
    WEIVEL_REFUSAL_UNDECLARED = 12,
    /**
     * A plugin reached for a service it binds to one provider while another owner provides it. Names the id reached
     * for.
     */
    WEIVEL_REFUSAL_WRONG_PROVIDER = 13,
    /** A plugin was stopped while it waited, or asked to wait after it stopped. Names the service id it waited for. */
    WEIVEL_REFUSAL_STOPPED = 14,
    /** A want's deadline passed before a service it accepts was registered. Names the service id it waited for. */
    WEIVEL_REFUSAL_TIMED_OUT = 15,
    /**
     * A constrained want spent its budget and no provider met its constraints: nothing provided the service, or it
     * does not degrade. Detail names the constraints and what was offered. Names the first of the want's constraints
     * that what its provider last offered does not meet, or its first constraint when nothing provides the service
     * or the offer meets them all.
     */
    WEIVEL_REFUSAL_UNMET_CONSTRAINT = 16,
    /**
     * A degradable want's provider would serve neither its constraints nor an unconstrained ask. Names the service id
     * it waited for.
     */
    WEIVEL_REFUSAL_NOT_SERVED = 17,
    /**
     * A plugin's activation provided fewer capabilities than its manifest declares, or more; detail names each one
     * missing or extra, and fix the first of them. Names the plugin's import name.
     */
    WEIVEL_REFUSAL_ACTIVATION_MISMATCH = 18,
    /** A plugin provided a capability it had already provided. Names the capability id. */
    WEIVEL_REFUSAL_PROVIDED_TWICE = 19
} weivel_refusal_code;

/**
 * A refusal, as a value. `detail` says what happened and `fix` what would make it succeed, both English UTF-8.
 * Both strings belong to the struct carrying the refusal until that struct's release.
 *
 * Every struct carrying a refusal also answers, after its last field, `subject`, what the refusal is about as its
 * code says, and `has_fix`, 1 when `fix` is non-empty and 0 otherwise, so a caller keys on what was refused without
 * reading English. A refusal whose code is NONE has a NULL subject and no fix.
 *
 * @implNote The two are appended to each carrier, never here: this struct is embedded by value ahead of later
 * fields (weivel_want_event's `degraded`) and inside an element that never grows (weivel_plugin_refusal), so growing
 * it would move fields already published.
 */
typedef struct weivel_refusal {
    uint32_t code;
    const char *detail;
    const char *fix;
} weivel_refusal;

/** How much a log event matters. */
typedef enum weivel_log_level {
    WEIVEL_LOG_DEFAULT = 0,
    WEIVEL_LOG_DEBUG = 1,
    WEIVEL_LOG_INFO = 2,
    WEIVEL_LOG_WARNING = 3,
    WEIVEL_LOG_ERROR = 4
} weivel_log_level;

/** A diagnostic from the core. `text` belongs to the event carrying it until weivel_event_release. */
typedef struct weivel_log_event {
    uint32_t level;
    const char *text;
} weivel_log_event;

/**
 * What an event reports. New kinds are only appended; a caller ignores, and still releases, a kind it does not know.
 */
typedef enum weivel_event_kind {
    /** No event: what weivel_next_event leaves when the queue is empty, and what release leaves behind. */
    WEIVEL_EVENT_NONE = 0,
    /** A diagnostic; the payload is `data.log`. */
    WEIVEL_EVENT_LOG = 1,
    /** A refusal; the payload is `data.refusal`, and `data.refused` reads it with its subject and has_fix. */
    WEIVEL_EVENT_REFUSAL = 2,
    /** A want settled, with a service or with a refusal, exactly once; the payload is `data.want`. */
    WEIVEL_EVENT_WANT_SETTLED = 3,
    /** A watched service was registered or unregistered; the payload is `data.watch`. */
    WEIVEL_EVENT_WATCH = 4,
    /** A service a plugin holds was withdrawn, so the plugin must stop using it; the payload is `data.revoked`. */
    WEIVEL_EVENT_REVOKED = 5,
    /** A plugin reached for what it may not, reported once per plugin, kind and id; the payload is `data.reach`. */
    WEIVEL_EVENT_REACH_REFUSED = 6,
    /**
     * A constrained want asks a provider's object a question, answered by weivel_report_capacity; the payload is
     * `data.capacity`.
     */
    WEIVEL_EVENT_CAPACITY_WANTED = 7,
    /** A watched capability gained or lost a verified implementation; the payload is `data.capability`. */
    WEIVEL_EVENT_CAPABILITY = 8,
    /**
     * The core's earliest deadline changed: one was set where none was, it moved earlier or later, or the last one
     * went. The payload is `data.deadline`, the deadline weivel_next_deadline answers from then on, so a caller arms
     * and disarms its timer from the queue alone and never keeps its own list of the calls that move a deadline.
     * Each call that leaves the earliest deadline where it found it queues none, however much it moved meanwhile, and
     * a change comes after every other event the same call queued. At most one waits: a newer one takes out any not
     * yet drained and is queued last, so what is drained is always the current deadline. Its room in the queue is
     * kept from the core's creation on, so it never needs memory and no call fails for want of it.
     */
    WEIVEL_EVENT_DEADLINE = 9
} weivel_event_kind;

/**
 * How a want settled. `implementation` is the registration handle of the service it settled with, or 0 when it was
 * rejected, and then `refusal` says why (WEIVEL_REFUSAL_UNDECLARED, WEIVEL_REFUSAL_WRONG_PROVIDER,
 * WEIVEL_REFUSAL_STOPPED, WEIVEL_REFUSAL_TIMED_OUT, WEIVEL_REFUSAL_UNMET_CONSTRAINT or WEIVEL_REFUSAL_NOT_SERVED); a
 * settled want's `refusal` is code NONE with NULL texts.
 */
typedef struct weivel_want_event {
    weivel_handle want;
    weivel_handle plugin;
    weivel_handle implementation;
    weivel_refusal refusal;
    /**
     * 1 when a constrained want accepted a provider that never met its constraints, because its budget was spent
     * and it is degradable; a WEIVEL_LOG_WARNING log saying what it asked for and what it was offered comes just
     * before. 0 otherwise. Appended after the event's first shape.
     */
    uint32_t degraded;
    /**
     * A rejected want's subject (see weivel_refusal); for a degraded one, the constraint it accepted unmet, chosen
     * as WEIVEL_REFUSAL_UNMET_CONSTRAINT chooses it, so the WARNING log's text need never be read; NULL for any
     * other settlement, and for a rejection whose refusal texts memory could not copy. Appended after `degraded`.
     */
    const char *subject;
    /** 1 when `refusal.fix` is non-empty, else 0, so 0 for every want not rejected. Appended after `subject`. */
    uint32_t has_fix;
} weivel_want_event;

/** What a watch heard. */
typedef enum weivel_change {
    WEIVEL_CHANGE_REGISTERED = 1,
    WEIVEL_CHANGE_UNREGISTERED = 2
} weivel_change;

/** One change a watch heard: `implementation` is the registration handle, `change` a weivel_change. */
typedef struct weivel_watch_event {
    weivel_handle watch;
    weivel_handle plugin;
    weivel_handle implementation;
    uint32_t change;
} weivel_watch_event;

/**
 * A withdrawal: `plugin` held `implementation`, a service's registration handle or a capability's implementation
 * handle, which is gone.
 */
typedef struct weivel_revoked_event {
    weivel_handle plugin;
    weivel_handle implementation;
} weivel_revoked_event;

/** What a reach refusal is about. */
typedef enum weivel_reach_kind {
    WEIVEL_REACH_SERVICE = 1,
    WEIVEL_REACH_CAPABILITY = 2
} weivel_reach_kind;

/** A reported refusal of reach: `plugin` reached for the `kind` with `id` and was refused. */
typedef struct weivel_reach_refusal {
    uint32_t kind;
    weivel_handle plugin;
    const char *id;
    weivel_refusal refusal;
    /** The refusal's subject, the same text as `id`. Appended after `refusal`. */
    const char *subject;
    /** 1 when `refusal.fix` is non-empty, else 0. Appended after `subject`. */
    uint32_t has_fix;
} weivel_reach_refusal;

/**
 * A refusal event's payload read with what it is about: `refusal` is the very `data.refusal` the event carries, and
 * the fields after it are appended to that payload.
 */
typedef struct weivel_refusal_event {
    weivel_refusal refusal;
    /** The refusal's subject (see weivel_refusal). */
    const char *subject;
    /** 1 when `refusal.fix` is non-empty, else 0. */
    uint32_t has_fix;
} weivel_refusal_event;

/** One named number: an attribute a service advertises, or a constraint a consumer sets on it. */
typedef struct weivel_number {
    uint32_t size;
    const char *name;
    double value;
} weivel_number;

/**
 * A list of size-prefixed elements, `count` of them contiguous at `items`. The first element's `size` is the
 * list's stride and every element declares the same, so elements built against an older or a newer header are read
 * correctly. `items` may be NULL only when `count` is 0. The other element lists read the same way.
 *
 * @implNote A list carries no `size` of its own because it is embedded by value: it could never grow without moving
 * the fields after it, so its elements are what grow.
 */
typedef struct weivel_numbers {
    const weivel_number *items;
    uint32_t count;
} weivel_numbers;

/** What a WEIVEL_EVENT_CAPACITY_WANTED asks of a provider's object. */
typedef enum weivel_capacity_question {
    /** What the provider offers now: answer with its live attributes, none at all when it has nothing live to add. */
    WEIVEL_CAPACITY_ATTRIBUTES = 1,
    /**
     * Whether the provider serves the consumer under `constraints`, an unconstrained ask when they are empty: answer
     * with `serves`.
     */
    WEIVEL_CAPACITY_SERVE = 2
} weivel_capacity_question;

/**
 * One question a constrained want puts to the provider registered as `registration`, on behalf of `plugin`.
 * `constraints` are the consumer's, each name and value, and live in the event's storage; empty for an
 * unconstrained ask. The answer names `want`, `registration` and `request`, and counts only until the poll or
 * question that asked ends.
 */
typedef struct weivel_capacity_event {
    weivel_handle want;
    weivel_handle plugin;
    weivel_handle registration;
    uint64_t request;
    /** A weivel_capacity_question. */
    uint32_t question;
    weivel_numbers constraints;
} weivel_capacity_event;

/** What a capability watch heard. */
typedef enum weivel_capability_change {
    /** An implementation joined: its plugin's activation was verified. */
    WEIVEL_CAPABILITY_PROVIDED = 1,
    /** An implementation left: its plugin was quarantined or released. */
    WEIVEL_CAPABILITY_WITHDRAWN = 2
} weivel_capability_change;

/**
 * One change a capability watch heard: `plugin` watches, `implementation` is the handle weivel_provide_capability
 * answered `provider`, and `change` is a weivel_capability_change. A withdrawal names the very implementation that
 * left, so a consumer drops exactly that one.
 */
typedef struct weivel_capability_event {
    weivel_handle watch;
    weivel_handle plugin;
    weivel_handle implementation;
    weivel_handle provider;
    uint32_t change;
} weivel_capability_event;

/**
 * The earliest deadline a WEIVEL_EVENT_DEADLINE announces: `set` is 1 with `at` the deadline in the caller's
 * milliseconds, or 0 with `at` 0 when nothing waits on time any more, so no timer is needed.
 */
typedef struct weivel_deadline_event {
    int64_t at;
    uint32_t set;
} weivel_deadline_event;

/**
 * How many 64-bit words an event's payload union spans. Part of the event's first published shape, so it never
 * shrinks; a payload that outgrows it enlarges the union and so `sizeof(weivel_event)`, which the size convention
 * absorbs.
 */
#define WEIVEL_EVENT_PAYLOAD_WORDS 16

/**
 * One event, filled by weivel_next_event into a struct the caller owns.
 *
 * Every string any payload carries lives in `storage` and stays valid until weivel_event_release, whatever other
 * calls are made meanwhile; the core keeps no pointer into the struct. `storage` is private to the core.
 */
typedef struct weivel_event {
    uint32_t size;
    uint32_t kind;
    void *storage;
    union {
        weivel_log_event log;
        weivel_refusal refusal;
        weivel_want_event want;
        weivel_watch_event watch;
        weivel_revoked_event revoked;
        weivel_reach_refusal reach;
        weivel_capacity_event capacity;
        weivel_capability_event capability;
        weivel_refusal_event refused;
        weivel_deadline_event deadline;
        uint64_t reserved[WEIVEL_EVENT_PAYLOAD_WORDS];
    } data;
} weivel_event;

/**
 * A list of strings: `count` pointers at `items`. `items` may be NULL only when `count` is 0, and no item may be
 * NULL. Read only during the call it is passed to.
 */
typedef struct weivel_strings {
    const char *const *items;
    uint32_t count;
} weivel_strings;

/**
 * A yes-or-no field a manifest may leave out. Zero is WEIVEL_CHOICE_DEFAULT, so a zeroed field, or one beyond the
 * caller's `size`, takes the default the field documents, as every other field does.
 */
typedef enum weivel_choice {
    WEIVEL_CHOICE_DEFAULT = 0,
    WEIVEL_CHOICE_NO = 1,
    WEIVEL_CHOICE_YES = 2
} weivel_choice;

/** A service a plugin registers, with what it advertises until the live service reports its own. */
typedef struct weivel_service_offer {
    uint32_t size;
    /** The service id. Required, opaque. */
    const char *id;
    /** Advertised attributes, no name twice. Appended after the offer's first shape. */
    weivel_numbers attributes;
} weivel_service_offer;

typedef struct weivel_service_offers {
    const weivel_service_offer *items;
    uint32_t count;
} weivel_service_offers;

/** A service a plugin asks for, and what its absence or its shortfall costs. */
typedef struct weivel_service_want {
    uint32_t size;
    /** The service id. Required, opaque. */
    const char *id;
    /** The identity of the one plugin to take it from, opaque; NULL means any provider of the id. */
    const char *provider;
    /** Whether the plugin still starts when nothing provides the service, a weivel_choice. Default YES. */
    uint32_t optional;
    /**
     * Whether the plugin accepts a provider that never satisfies `constraints` once the budget is spent, a
     * weivel_choice. Default NO. It never excuses an absence.
     */
    uint32_t degradable;
    /** What the plugin needs of the service, no name twice. Appended after the want's first shape. */
    weivel_numbers constraints;
} weivel_service_want;

typedef struct weivel_service_wants {
    const weivel_service_want *items;
    uint32_t count;
} weivel_service_wants;

/** The capabilities a plugin provides and consumes, as opaque ids, none listed twice. */
typedef struct weivel_capability_declarations {
    weivel_strings provides;
    weivel_strings consumes;
} weivel_capability_declarations;

/** The services a plugin provides and consumes, no id listed twice in either. */
typedef struct weivel_service_declarations {
    weivel_service_offers provides;
    weivel_service_wants consumes;
} weivel_service_declarations;

/**
 * A plugin's manifest, as a value the caller builds; the core parses no file. Every string is opaque to the core
 * and read only during the call the manifest is passed to: a call that keeps it copies it first.
 *
 * Its own shape is checked (a non-empty import name, no NULL in a list, no id listed twice); what a valid id or
 * target looks like is the standard's business, not the core's. A field beyond the caller's `size` is absent, so a
 * manifest built against an older header loads, and one built against a newer header is read up to what this core
 * knows.
 */
typedef struct weivel_manifest {
    uint32_t size;
    /** The plugin's import name, its id. Required. */
    const char *import_name;
    /** The api level the plugin needs at least. */
    uint32_t api;
    /** The plugin's entry, opaque; NULL when it declares none. */
    const char *entry;
    /** The applications the plugin is built for, opaque; empty means any application. */
    weivel_strings targets;
    weivel_capability_declarations capabilities;
    weivel_service_declarations services;
    /** The permissions the plugin asks for, opaque. Appended after the manifest's first shape. */
    weivel_strings permissions;
    /**
     * Who the plugin is, opaque, supplied by whatever loaded it rather than written by the plugin: the name a
     * consumer's `provider` binds to. NULL means its import name; an empty one is malformed. Fixed for the life of
     * the registration. Appended after `permissions`.
     */
    const char *identity;
} weivel_manifest;

/** Which way a constraint bounds the attribute it is compared against. */
typedef enum weivel_bound {
    /** The attribute must be at least the constraint's value. */
    WEIVEL_BOUND_LOWER = 1,
    /** The attribute must be at most the constraint's value. */
    WEIVEL_BOUND_UPPER = 2
} weivel_bound;

/** One entry of a host's constraint vocabulary: `constraint_name` is met by `attribute_name` within `bound`. */
typedef struct weivel_constraint_entry {
    uint32_t size;
    /** Required, non-empty, named by no other entry. */
    const char *constraint_name;
    /** Required, non-empty. */
    const char *attribute_name;
    /** A weivel_bound. Required. */
    uint32_t bound;
} weivel_constraint_entry;

/** A list of vocabulary entries, read the way weivel_numbers is read. */
typedef struct weivel_constraint_vocabulary {
    const weivel_constraint_entry *items;
    uint32_t count;
} weivel_constraint_vocabulary;

/** The api level this core implements, which a host implements unless it says otherwise. */
#define WEIVEL_API_VERSION 2u

/** The largest `initial_event_capacity` weivel_core_create accepts. */
#define WEIVEL_CORE_MAXIMUM_INITIAL_EVENT_CAPACITY 65536u

/** Options for weivel_core_create. Zero, or a field beyond `size`, means the default. */
typedef struct weivel_core_options {
    uint32_t size;
    /** How many events the queue holds before it first grows. Default 16; above
     * WEIVEL_CORE_MAXIMUM_INITIAL_EVENT_CAPACITY the call is malformed. */
    uint32_t initial_event_capacity;
    /** The least level of log event the core queues, a weivel_log_level. Default WEIVEL_LOG_WARNING. */
    uint32_t minimum_log_level;
    /** The highest api level this host implements. Default WEIVEL_API_VERSION. */
    uint32_t api;
    /** The applications this host hosts, opaque, copied by weivel_core_create; empty means every application. */
    weivel_strings applications;
    /**
     * The service ids any plugin may register, because they name a contract rather than one plugin's offering,
     * copied by weivel_core_create. Every other id a plugin registers carries its import name as namespace.
     */
    weivel_strings well_known_services;
    /**
     * Which constraint name compares against which attribute, and how, copied by weivel_core_create. The core
     * knows no constraint of its own, so a constraint no entry names is never met. Appended after
     * `well_known_services`.
     */
    weivel_constraint_vocabulary constraint_vocabulary;
} weivel_core_options;

/**
 * Creates a core.
 *
 * @param options may be NULL for every default; read only during the call.
 * @param core receives the new core, owned by the caller until weivel_core_destroy. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK, or WEIVEL_STATUS_MALFORMED (no `core`, options smaller than their first shape, an
 * initial_event_capacity above its maximum, an `applications` or `well_known_services` list that cannot be read, or
 * a vocabulary that cannot be read, has an entry missing a name or with a bound that is no weivel_bound, or names
 * a constraint twice) or
 * WEIVEL_STATUS_OUT_OF_MEMORY, with `*core` set to NULL when `core` is given. No core exists to carry a refusal event
 * for either.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_core_create(const weivel_core_options *options, weivel_core **core,
                                            uint32_t *pending_events);

/**
 * Destroys a core and every event still queued in it. Every handle it issued becomes meaningless. Events already
 * drained stay valid until their own release.
 *
 * @return WEIVEL_STATUS_OK, also for a NULL core, which is a no-op.
 * Threading: no other call on this core may be in flight or follow.
 */
WEIVEL_API weivel_status weivel_core_destroy(weivel_core *core);

/**
 * Moves the oldest queued event into `event`, which the caller owns. Events come out once each, in the order they
 * were queued.
 *
 * @param event the caller's struct with `size` set and kind WEIVEL_EVENT_NONE, as WEIVEL_INIT or a release leaves
 * it; on WEIVEL_STATUS_OK it holds the event and must be passed to weivel_event_release.
 * @param pending_events receives how many events still wait; may be NULL.
 * @return WEIVEL_STATUS_OK with an event; WEIVEL_STATUS_NONE when none waits, leaving `event` empty with nothing to
 * release; WEIVEL_STATUS_MALFORMED for a NULL core or event, a size below the first shape, or an event whose kind is
 * not WEIVEL_EVENT_NONE (an unreleased event is never overwritten), queuing a refusal when the core is given.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_next_event(weivel_core *core, weivel_event *event, uint32_t *pending_events);

/**
 * Frees what an event carries and leaves it WEIVEL_EVENT_NONE.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for a NONE event or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the event's first shape.
 * Threading: any thread; needs no core, and may run after the core that queued it is destroyed.
 */
WEIVEL_API weivel_status weivel_event_release(weivel_event *event);

/**
 * Answers the earliest deadline the core is waiting on, so the caller can arm its own timer for it. It is the very
 * deadline the last WEIVEL_EVENT_DEADLINE announced, read without draining the queue; a caller arming from those
 * events needs it only to look.
 *
 * @param deadline receives the deadline in the caller's milliseconds when one is set. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK with `*deadline` set, or WEIVEL_STATUS_NONE when nothing waits on time, so no timer is
 * needed.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_next_deadline(weivel_core *core, int64_t *deadline, uint32_t *pending_events);

/**
 * Tells the core the time is now `now`, settling every deadline at or before it, and nothing after it.
 *
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK, or WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_advance(weivel_core *core, int64_t now, uint32_t *pending_events);

/** Which compatibility check refused a manifest. */
typedef enum weivel_axis {
    /** None did: the manifest is supported, or its own shape was refused before any check ran. */
    WEIVEL_AXIS_NONE = 0,
    /** The manifest targets applications this host does not host. Checked first. */
    WEIVEL_AXIS_APPLICATION = 1,
    /** The manifest needs a higher api than this host implements. Checked second. */
    WEIVEL_AXIS_API = 2
} weivel_axis;

/**
 * The answer to weivel_supports, filled into a struct the caller owns. When `supported` is 0, `refusal` says why
 * (code WEIVEL_REFUSAL_INCOMPATIBLE with `axis` set, or WEIVEL_REFUSAL_MALFORMED_MANIFEST with axis NONE); when it
 * is 1, `refusal` is code NONE with NULL texts. The texts live in `storage`, private to the core, until
 * weivel_support_release.
 */
typedef struct weivel_support {
    uint32_t size;
    uint32_t supported;
    /** A weivel_axis. */
    uint32_t axis;
    weivel_refusal refusal;
    void *storage;
    /** The refusal's subject (see weivel_refusal), NULL when supported. Appended after `storage`. */
    const char *subject;
    /** 1 when `refusal.fix` is non-empty, else 0. Appended after `subject`. */
    uint32_t has_fix;
} weivel_support;

/**
 * Decides whether this host can load the manifest's plugin, against the applications and api given to
 * weivel_core_create. This is the one place compatibility is decided: the application first, then the api floor.
 * Language never refuses. An incompatible or malformed manifest is an answer, not a failure, and queues nothing.
 *
 * @param manifest read only during the call. Required.
 * @param answer the caller's struct with `size` set and nothing held, as WEIVEL_INIT or a release leaves it; on
 * WEIVEL_STATUS_OK it holds the answer and must be passed to weivel_support_release.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK with an answer; WEIVEL_STATUS_MALFORMED for a NULL core, manifest or answer, a size below
 * either struct's first shape, or an answer still holding one, queuing a refusal when the core is given;
 * WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_supports(weivel_core *core, const weivel_manifest *manifest, weivel_support *answer,
                                         uint32_t *pending_events);

/**
 * Frees what an answer carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty answer or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the answer's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_support_release(weivel_support *answer);

/**
 * Registers a plugin by its manifest, which the core copies whole before returning, so the caller may free its own
 * at once. Registering checks the manifest's shape only, never its compatibility, which weivel_supports answers.
 *
 * @param manifest read only during the call. Required.
 * @param plugin receives the plugin's handle. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK with `*plugin` set; WEIVEL_STATUS_REFUSED for a malformed manifest, queuing a refusal
 * with code WEIVEL_REFUSAL_MALFORMED_MANIFEST, or for an import name a registered plugin already holds, since the
 * import name namespaces a plugin's services, queuing one with code WEIVEL_REFUSAL_NAME_TAKEN, or for an identity
 * one already holds, since a consumer's `provider` binds to it, queuing WEIVEL_REFUSAL_IDENTITY_TAKEN;
 * WEIVEL_STATUS_MALFORMED for a NULL core, manifest or plugin or a manifest smaller than its first shape, queuing a
 * refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
 * `*plugin` is 0 on every answer but OK.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_register_manifest(weivel_core *core, const weivel_manifest *manifest,
                                                  weivel_handle *plugin, uint32_t *pending_events);

/**
 * Which declarations draw the edges an activation plan orders by. There is no default: the host chooses, because a
 * capability implies order on one tier and does not on another. Declarations of the other kind are ignored
 * entirely, for ordering and for refusal alike.
 */
typedef enum weivel_edge_source {
    /** A plugin's provided and consumed capabilities. A consumed capability is always optional. */
    WEIVEL_EDGES_CAPABILITIES = 1,
    /** A plugin's provided and consumed services, each consumed one optional or not as its manifest says. */
    WEIVEL_EDGES_SERVICES = 2
} weivel_edge_source;

/** One edge of an activation plan: `provider` activates before `consumer`. */
typedef struct weivel_edge {
    weivel_handle provider;
    weivel_handle consumer;
} weivel_edge;

/** One cycle of an activation plan: at least two plugins that each wait, directly or not, on all the others. */
typedef struct weivel_cycle {
    const weivel_handle *members;
    uint32_t count;
} weivel_cycle;

/** A plugin an activation plan will not start, and why. */
typedef struct weivel_plugin_refusal {
    weivel_handle plugin;
    weivel_refusal refusal;
} weivel_plugin_refusal;

/** What one of an activation plan's refusals is about: its subject (see weivel_refusal) and whether it has a fix. */
typedef struct weivel_refusal_subject {
    const char *subject;
    /** 1 when the refusal's `fix` is non-empty, else 0. */
    uint32_t has_fix;
} weivel_refusal_subject;

/**
 * The answer to weivel_plan_activation, filled into a struct the caller owns. Every array and text lives in
 * `storage`, private to the core, until weivel_activation_plan_release; an empty array is NULL with count 0.
 *
 * - `order`: the plugins to start, each after every plugin it has an edge from.
 * - `edges`: provider and consumer pairs between plugins that will be considered for start (refused plugins draw
 *   none), consumers in input order and each one's providers in the order its needs and the input name them.
 * - `cycles`: each set of plugins that need each other. Its members are neither refused nor in `order`; a plugin
 *   that needs one starts without it when the need is optional and is refused when it is mandatory.
 * - `refusals`: the plugins that will not start: a handle that names no plugin (WEIVEL_REFUSAL_STALE_HANDLE), a
 *   plugin listed again (WEIVEL_REFUSAL_REPEATED_PLUGIN), then each one whose mandatory need nothing meets
 *   (WEIVEL_REFUSAL_UNMET_NEED), in the order they were found.
 * - `refusal_subjects`: one entry per refusal, at the same index, `refusal_count` of them, appended after `storage`.
 *
 * Every array here is of the kind whose elements never grow (see Answers at the head of this file): a caller indexes
 * them at its own compiled stride, and anything a later version adds to a plan arrives as a new array appended to
 * this struct.
 */
typedef struct weivel_activation_plan {
    uint32_t size;
    const weivel_handle *order;
    uint32_t order_count;
    const weivel_edge *edges;
    uint32_t edge_count;
    const weivel_cycle *cycles;
    uint32_t cycle_count;
    const weivel_plugin_refusal *refusals;
    uint32_t refusal_count;
    void *storage;
    const weivel_refusal_subject *refusal_subjects;
} weivel_activation_plan;

/**
 * Plans the activation of registered plugins: a provider before its consumers, cycles and refusals as data.
 *
 * A plugin's offers and needs are read from the manifest it was registered with, of the kind `edge_source` names.
 * A need any other plugin of the set offers draws an edge from that plugin to it; one only the host offers draws
 * none. A mandatory need nothing in the set or the host meets refuses its plugin; an optional one waits; whether a
 * need is degradable never excuses an absence. A refused plugin stops being a provider, and so does a cycle member,
 * so refusal runs until a pass refuses nothing more. A cycle member is reported in its cycle, not refused.
 *
 * The order is built in sweeps over `plugins` as given: each sweep places, in input order, every plugin whose
 * providers are all placed or in a cycle, and sweeps repeat until one places nothing. The host's own services count
 * as placed from the end of the first sweep, as though the host were listed last. So the same input always answers
 * the same order, the one the reference engine answers.
 *
 * @param plugins handles weivel_register_manifest answered, read only during the call; may be NULL when `count` is
 * 0.
 * @param edge_source a weivel_edge_source.
 * @param host_provided service ids the host supplies itself; may be NULL for none, read only during the call. They
 * meet a need under WEIVEL_EDGES_SERVICES as a plugin's offer would, never appear in the plan, and are ignored
 * under WEIVEL_EDGES_CAPABILITIES.
 * @param plan the caller's struct with `size` set and nothing held, as WEIVEL_INIT or a release leaves it; on
 * WEIVEL_STATUS_OK it holds the plan and must be passed to weivel_activation_plan_release.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK with a plan, including one that refuses or cycles, which queues nothing;
 * WEIVEL_STATUS_MALFORMED for a NULL core or plan, a plan below its first shape or still holding one, NULL plugins
 * with a count, an edge source that is no weivel_edge_source, or a host_provided list that cannot be read, queuing
 * a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_plan_activation(weivel_core *core, const weivel_handle *plugins, uint32_t count,
                                                uint32_t edge_source, const weivel_strings *host_provided,
                                                weivel_activation_plan *plan, uint32_t *pending_events);

/**
 * Frees what a plan carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty plan or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the plan's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_activation_plan_release(weivel_activation_plan *plan);

/**
 * The host itself: a reserved handle no plugin is ever issued.
 *
 * As an owner, a service registered under it is exempt from namespacing, owned by no plugin so no release or
 * quarantine takes it away, and registering its id again replaces it.
 *
 * As an asker, it may be passed as the plugin of weivel_get_service, weivel_want_service, weivel_watch_service,
 * weivel_all_capabilities and weivel_watch_capability. The host is never fenced, so it reaches every id, declared by
 * no manifest, and is never refused or reported for reach; it is never late; it holds what it is handed and is
 * revoked it, by a WEIVEL_EVENT_REVOKED whose `plugin` is WEIVEL_HOST, exactly as a plugin is; and since no plugin
 * stops it, its watches and wants end only by weivel_unregister or when the core is destroyed. It has no ledger row.
 */
#define WEIVEL_HOST ((weivel_handle)UINT64_MAX)

/** How a plugin is opened. Zero, or a field beyond `size`, means the default. */
typedef struct weivel_plugin_options {
    uint32_t size;
    /** The caller's own token for the plugin, carried through uninspected and answered by weivel_plugin_payload. */
    uint64_t payload;
} weivel_plugin_options;

/**
 * Opens a registered plugin, so it may register, get, want and watch services. Opening a quarantined plugin again
 * lifts its fence; its identity is the one its manifest was registered with, always.
 *
 * @param plugin a handle weivel_register_manifest answered.
 * @param options may be NULL for every default; read only during the call.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle (WEIVEL_REFUSAL_STALE_HANDLE) or a plugin
 * open already or released (WEIVEL_REFUSAL_PLUGIN_STATE), each queuing a refusal; WEIVEL_STATUS_MALFORMED for a
 * NULL core or options below their first shape.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_open_plugin(weivel_core *core, weivel_handle plugin,
                                            const weivel_plugin_options *options, uint32_t *pending_events);

/**
 * Answers the payload a plugin was last opened with, the very value given: the core never reads it. 0 for a plugin
 * never opened.
 *
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle; WEIVEL_STATUS_MALFORMED for a NULL core or
 * payload.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_plugin_payload(weivel_core *core, weivel_handle plugin, uint64_t *payload,
                                               uint32_t *pending_events);

/**
 * Releases a plugin, in one call: every capability implementation it provided is withdrawn, revoked to each holder
 * by a WEIVEL_EVENT_REVOKED and heard by each watch of that capability as WEIVEL_CAPABILITY_WITHDRAWN when it was
 * verified, its own watches included; then its watches stop, everything it registered is unregistered (each holder
 * told by a WEIVEL_EVENT_REVOKED and each watch by a WEIVEL_EVENT_WATCH), every want it still waits on is rejected
 * with WEIVEL_REFUSAL_STOPPED, and every service and capability implementation it holds is revoked to it. Its
 * import name and identity are free again at once, so a plugin of the same name may be registered.
 *
 * The handle stays valid until weivel_forget_plugin, so a call the released plugin still makes is late: a `get` is
 * let through and starts a new holding, a `want` is rejected, a `watch` or a registration is refused with a log
 * only. Releasing a released plugin does nothing.
 *
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle; WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_release_plugin(weivel_core *core, weivel_handle plugin, uint32_t *pending_events);

/**
 * Why a plugin was quarantined, as the caller knows it: its activation threw, or anything else the host decided.
 * Zero, or a field beyond `size`, means the default.
 */
typedef struct weivel_quarantine_reason {
    uint32_t size;
    /** A weivel_refusal_code, or WEIVEL_REFUSAL_NONE when no code fits; carried as given. */
    uint32_t code;
    /** What happened, English UTF-8, copied. Required. */
    const char *detail;
    /** What would make it succeed, English UTF-8, copied. Required. */
    const char *fix;
} weivel_quarantine_reason;

/**
 * Quarantines a plugin: everything weivel_release_plugin takes from it goes in the same one call, but it keeps its
 * import name and identity, and opening it again lifts the fence. Its calls meanwhile are late, as a released
 * plugin's are. An INFO log names the plugin and why.
 *
 * @param reason why, shown by the plugin's ledger row until it is opened again; read only during the call. NULL
 * gives none, and the row then shows the refusal weivel_verify_activation last answered its activation, if any. A
 * reason given replaces any shown before.
 * @return WEIVEL_STATUS_OK, also for a plugin quarantined already; WEIVEL_STATUS_REFUSED for a stale handle or a
 * released plugin (WEIVEL_REFUSAL_PLUGIN_STATE); WEIVEL_STATUS_MALFORMED for a NULL core, or a reason below its
 * first shape or missing its detail or fix, queuing a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY,
 * with nothing changed.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_quarantine_plugin(weivel_core *core, weivel_handle plugin,
                                                  const weivel_quarantine_reason *reason, uint32_t *pending_events);

/**
 * Forgets a released plugin: its record, its manifest, the refusals reported for it and its handle all go, and
 * whatever it still holds through late calls is revoked to it first. The handle then names nothing, so a call made
 * with it afterwards is refused with WEIVEL_REFUSAL_STALE_HANDLE, and its slot may name something new later.
 *
 * Each such revocation is a WEIVEL_EVENT_REVOKED whose `plugin` is this very handle, already stale when the event
 * is drained: it tells the caller which of its own entries to drop, and is never a handle to call with.
 *
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle or a plugin not released
 * (WEIVEL_REFUSAL_PLUGIN_STATE), queuing a refusal; WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_forget_plugin(weivel_core *core, weivel_handle plugin, uint32_t *pending_events);

/**
 * Registers a service. The core holds no object: the registration handle answered here stands for the
 * implementation, the caller keeps the object it names, and every `get`, settled `want` and watch answers this
 * handle. It is also the means to unregister, through weivel_unregister.
 *
 * A plugin registers an id in its own namespace, `<import name>:<name>`, or a well-known id, and only an id no
 * other owner holds. A registration is never checked against the plugin's declarations. Every want waiting on the
 * id that accepts this owner settles with it, in the order they asked, and every watch of the id that accepts it
 * hears it; a want bound to another provider keeps waiting, and its mismatch is reported once.
 *
 * @param owner a plugin handle, or WEIVEL_HOST for a service the host itself supplies.
 * @param id the service id, copied. Required, non-empty.
 * @param registration receives the registration handle. Required; 0 on every answer but OK.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale owner, a plugin never opened
 * (WEIVEL_REFUSAL_PLUGIN_STATE), an id outside its namespace (WEIVEL_REFUSAL_OUTSIDE_NAMESPACE) or one already
 * held (WEIVEL_REFUSAL_SERVICE_TAKEN), each queuing a refusal, or a late registration from a plugin no longer
 * running, queuing a log only; WEIVEL_STATUS_MALFORMED for a NULL core, id or registration or an empty id;
 * WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_register_service(weivel_core *core, weivel_handle owner, const char *id,
                                                 weivel_handle *registration, uint32_t *pending_events);

/**
 * Ends what a handle stands for: a registration is unregistered, a watch of a service or of a capability stops, and
 * a want still waiting is withdrawn without settling.
 *
 * Unregistering revokes the service from everyone who holds it, each by a WEIVEL_EVENT_REVOKED. A plugin holds a
 * registration from the moment the core hands it that registration's handle, by a `get`, a settled `want` or a
 * watch hearing it registered, until the registration ends or the plugin is released or quarantined. Each holder
 * is told once however often it resolved the service. Then every watch of the id that accepts the owner hears it
 * unregistered.
 *
 * @param handle a registration, watch or want handle.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a handle that names none of them any more
 * (WEIVEL_REFUSAL_STALE_HANDLE), queuing a refusal; WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_unregister(weivel_core *core, weivel_handle handle, uint32_t *pending_events);

/** Where a want or a watch stands, as weivel_handle_state answers it. */
typedef enum weivel_handle_condition {
    /** A want that has not settled and was not withdrawn. */
    WEIVEL_HANDLE_WAITING = 1,
    /** A want that settled, with a service or with a refusal: its WEIVEL_EVENT_WANT_SETTLED is queued or drained. */
    WEIVEL_HANDLE_SETTLED = 2,
    /** A want weivel_unregister withdrew before it settled. */
    WEIVEL_HANDLE_WITHDRAWN = 3,
    /** A watch of a service or of a capability that still hears changes. */
    WEIVEL_HANDLE_WATCHING = 4,
    /** A watch that ended: by weivel_unregister, or because its plugin was released or quarantined. */
    WEIVEL_HANDLE_STOPPED = 5
} weivel_handle_condition;

/**
 * Answers where a want or a watch stands, for any asker: it queues nothing and changes nothing, so reading it never
 * disturbs what it reads. A want or watch that has ended is answered how it ended until the next want, watch or
 * other record that takes its place in the core's handle table has ended in turn; after that its handle is refused
 * as a stale one is.
 *
 * @param handle a want handle weivel_want_service answered, or a watch handle weivel_watch_service or
 * weivel_watch_capability answered.
 * @param condition receives a weivel_handle_condition. Required; 0 on every answer but OK.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED, queuing nothing, for a handle that names no want or watch the
 * core remembers, a plugin or registration handle included, since a query is not an attempt; WEIVEL_STATUS_MALFORMED
 * for a NULL core or condition, queuing a refusal when the core is given.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_handle_state(weivel_core *core, weivel_handle handle, uint32_t *condition,
                                             uint32_t *pending_events);

/**
 * Answers the service registered under `id` now, as its registration handle, for a plugin that may reach it, or
 * for the host (WEIVEL_HOST), which reaches every id.
 *
 * Reach is checked in this order: the plugin must list `id` among the services it consumes or provides
 * (WEIVEL_REFUSAL_UNDECLARED); then, when it binds `id` to a provider and another owner holds it, it is refused
 * (WEIVEL_REFUSAL_WRONG_PROVIDER); then a plugin no longer running is late, and a late `get` is let through. A
 * refusal is reported, a WEIVEL_EVENT_REACH_REFUSED, once per plugin and id. The plugin then holds what it got,
 * a released plugin too: its late `get` starts a new holding, revoked to it when the service goes or the plugin is
 * forgotten.
 *
 * @param implementation receives the registration handle, or 0. Required.
 * @return WEIVEL_STATUS_OK with a service; WEIVEL_STATUS_NONE when nothing is registered under `id`, which is
 * absence, not a refusal; WEIVEL_STATUS_REFUSED for a refusal of reach, a stale plugin or one never opened;
 * WEIVEL_STATUS_MALFORMED for a NULL core, id or implementation or an empty id; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_get_service(weivel_core *core, weivel_handle plugin, const char *id,
                                            weivel_handle *implementation, uint32_t *pending_events);

/** How long a want may wait. Zero, or a field beyond `size`, means the default: no deadline at all. */
typedef struct weivel_want_options {
    uint32_t size;
    /**
     * Whether the want has a deadline, a weivel_choice. Default NO, so it waits for as long as it takes. A
     * constrained want's budget is its deadline, so it never reads this.
     */
    uint32_t timed;
    /**
     * The caller's time now, on its own monotonic scale in milliseconds; read when `timed` is YES, and for every
     * constrained want, whose polls it starts.
     */
    int64_t now;
    /** How long after `now` the want gives up, at least 0; read only when `timed` is YES. */
    int64_t timeout_millis;
    /**
     * Whether a constrained want has a budget, a weivel_choice, read only for one. Default NO, so it polls for as
     * long as it takes: it is never refused for time, and never degrades, since its budget is never spent. Appended
     * after `timeout_millis`.
     */
    uint32_t budgeted;
    /** How long a constrained want polls before it degrades or is refused, at least 0; read only when `budgeted`. */
    int64_t budget_millis;
    /**
     * How often a constrained want polls, at least 0. Required for a constrained want, which the core never gives a
     * default: 0, or options of an earlier shape, make that want malformed.
     */
    int64_t poll_millis;
} weivel_want_options;

/**
 * Asks for the service registered under `id`, now or whenever it arrives. Every want settles at most once, by one
 * WEIVEL_EVENT_WANT_SETTLED carrying the want handle: with the service when one it accepts is registered, which the
 * plugin then holds, or with a refusal. Without a deadline it never settles until then. With one it settles
 * either way exactly once: weivel_advance at or past `now + timeout_millis` rejects it with
 * WEIVEL_REFUSAL_TIMED_OUT, and weivel_next_deadline reports the earliest such moment.
 *
 * Reach is checked as weivel_get_service checks it, and a refusal rejects the want at once; the host (WEIVEL_HOST)
 * asks unfenced and is never late. A late want, from a plugin no longer running, is rejected with
 * WEIVEL_REFUSAL_STOPPED and logged. A want the plugin still waits on
 * when it is released or quarantined is rejected the same way.
 *
 * A want is constrained when the plugin's manifest gives `id` constraints. It never settles on a registration
 * alone: it polls, because a provider's capacity is live and the core never calls out to read it. Poll k starts at
 * `now + k * poll_millis` and lasts until the next one starts. Without a budget it polls without end. With one,
 * the last poll is the first whose start is `budget_millis` or more after `now`. At each poll:
 *
 * 1. When a registration it accepts is under `id`, a WEIVEL_EVENT_CAPACITY_WANTED asks that provider for its
 *    attributes. The answer is laid over what the provider's manifest advertises for `id`, name by name, so a live
 *    attribute replaces one claim and leaves the others standing, and an empty answer leaves every claim standing.
 *    Nothing is carried from one poll to the next: a poll the provider does not answer in time is "not right now".
 * 2. When those attributes meet every constraint, under the host's vocabulary, a second question asks whether the
 *    provider serves the consumer under them, and a yes settles the want with that registration.
 * 3. After the last poll of a budgeted want the budget is spent. A want that does not degrade, or whose service
 *    nothing provides, is rejected with WEIVEL_REFUSAL_UNMET_CONSTRAINT. A degradable one asks once more under its
 *    constraints, then unconstrained, each question answerable for `poll_millis`, and settles degraded with the first
 *    yes, or is rejected with WEIVEL_REFUSAL_NOT_SERVED.
 *
 * Every end of a poll or question left unanswered is a deadline weivel_next_deadline reports and weivel_advance
 * acts on; a poll whose time has wholly passed by then is skipped, never asked late. A constraint the vocabulary
 * does not name is logged once, at the want's start, and is never met.
 *
 * @param options may be NULL for no deadline, except for a constrained want, which needs `poll_millis`; read only
 * during the call.
 * @param want receives the want handle, which names the want until it settles. Required.
 * @return WEIVEL_STATUS_OK with a want, including one rejected at once; WEIVEL_STATUS_REFUSED for a stale plugin
 * or one never opened; WEIVEL_STATUS_MALFORMED for a NULL core, id or want, an empty id, options below their first
 * shape, `timed` or `budgeted` no weivel_choice, a negative timeout, budget or poll interval, or a constrained want
 * with no poll interval; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_want_service(weivel_core *core, weivel_handle plugin, const char *id,
                                             const weivel_want_options *options, weivel_handle *want,
                                             uint32_t *pending_events);

/** A provider's answer to one WEIVEL_EVENT_CAPACITY_WANTED. Zero, or a field beyond `size`, means the default. */
typedef struct weivel_capacity {
    uint32_t size;
    /** The `request` of the event being answered. Required. */
    uint64_t request;
    /**
     * For WEIVEL_CAPACITY_ATTRIBUTES: what the provider offers now, no name twice, read only during the call.
     * Empty means nothing live to add, so what it advertised stands.
     */
    weivel_numbers attributes;
    /** For WEIVEL_CAPACITY_SERVE: whether it serves, a weivel_choice; anything but YES is a no. */
    uint32_t serves;
} weivel_capacity;

/**
 * Answers a WEIVEL_EVENT_CAPACITY_WANTED. The core decides on it at once: it may settle the want, ask the provider
 * its next question, or wait for the next poll.
 *
 * An answer counts only for the question it names, while that question is open: from its event until its poll or
 * question ends, and a `now` at or after that end means it has. An answer to an earlier poll, to a question already
 * answered, for a registration that has since gone, for a want that has settled, or for a want that is not
 * constrained counts for nothing.
 *
 * A yes is acted on only once the consumer's holding of the service is recorded. When memory for it runs out the
 * call answers WEIVEL_STATUS_OUT_OF_MEMORY, nothing changes, and the question stays open to be answered again.
 *
 * @param want the want the event named.
 * @param registration the registration the event named.
 * @param capacity the answer. Required; read only during the call.
 * @param now the caller's time now.
 * @return WEIVEL_STATUS_OK when the answer counted; WEIVEL_STATUS_NONE when it answers no open question, a want
 * already settled or withdrawn included, as for a registration that has gone, queuing nothing;
 * WEIVEL_STATUS_MALFORMED for a NULL core or capacity, a capacity below its first shape or attributes that
 * cannot be read or name one twice, queuing a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_report_capacity(weivel_core *core, weivel_handle want, weivel_handle registration,
                                                const weivel_capacity *capacity, int64_t now,
                                                uint32_t *pending_events);

/**
 * Watches `id`: each later registration and unregistration of it that the plugin accepts is a WEIVEL_EVENT_WATCH,
 * and nothing registered before the watch is replayed. A watch bound to another provider skips its registrations
 * without a report. The watch lasts until weivel_unregister or until the plugin is released or quarantined.
 *
 * Reach is checked as weivel_get_service checks it, and a refused watch is inert; the host (WEIVEL_HOST) watches
 * unfenced. A late watch is inert and logged only, never reported.
 *
 * @param watch receives the watch handle. Required; 0 on every answer but OK.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a refusal of reach, a late watch, a stale plugin or one never
 * opened; WEIVEL_STATUS_MALFORMED for a NULL core, id or watch or an empty id; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_watch_service(weivel_core *core, weivel_handle plugin, const char *id,
                                              weivel_handle *watch, uint32_t *pending_events);

/**
 * A list of service ids, filled into a struct the caller owns. The ids live in `storage`, private to the core,
 * until weivel_service_list_release.
 */
typedef struct weivel_service_list {
    uint32_t size;
    weivel_strings ids;
    void *storage;
} weivel_service_list;

/**
 * Lists the ids a plugin may reach that are registered now, in the order its manifest declares them, consumed
 * before provided. A declared id nothing provides is not listed, and lateness is ignored. Listing is not an
 * attempt, so nothing is reported.
 *
 * @param list the caller's struct with `size` set and nothing held; on WEIVEL_STATUS_OK it must be passed to
 * weivel_service_list_release.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale plugin or one never opened; WEIVEL_STATUS_MALFORMED
 * for a NULL core or list, a list below its first shape or still holding one; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_service_ids(weivel_core *core, weivel_handle plugin, weivel_service_list *list,
                                            uint32_t *pending_events);

/**
 * Lists the services a registered plugin consumes that the core cannot tie to one provider, in its manifest's
 * order: a namespaced id (one holding a `:`) with no provider. A bare id names a host or shared service, which has
 * no plugin to bind to, so it is never listed.
 *
 * @return as weivel_service_ids answers, except that the plugin need not have been opened.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_unbound_services(weivel_core *core, weivel_handle plugin,
                                                 weivel_service_list *list, uint32_t *pending_events);

/**
 * Frees what a list carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty list or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the list's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_service_list_release(weivel_service_list *list);

/**
 * Provides a capability: the implementation handle answered here stands for the plugin's object, which the caller
 * keeps. It reaches no consumer yet: it joins the answers for `id` only once weivel_verify_activation passes the
 * plugin, so a consumer is never handed an implementation the host may still refuse. Providing is not checked
 * against the manifest here; weivel_verify_activation checks it.
 *
 * @param id the capability id, copied. Required, non-empty.
 * @param implementation receives the implementation handle. Required; 0 on every answer but OK.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale plugin or one never opened, for an id this plugin
 * already provides (WEIVEL_REFUSAL_PROVIDED_TWICE), each queuing a refusal, or for a plugin no longer running,
 * queuing a log only; WEIVEL_STATUS_MALFORMED for a NULL core, id or implementation or an empty id;
 * WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_provide_capability(weivel_core *core, weivel_handle plugin, const char *id,
                                                   weivel_handle *implementation, uint32_t *pending_events);

/**
 * Checks a plugin's activation: every capability its manifest provides must have been provided, and nothing else.
 * When it passes, each implementation the plugin provided joins the answers for its capability, after every one
 * verified before it, and each watch of that capability hears WEIVEL_CAPABILITY_PROVIDED. A capability provided
 * after a passing verify is answered OK with a handle that never joins: it is undeclared, so the next verify refuses
 * the plugin.
 *
 * When it does not pass, nothing joins, and the caller quarantines the plugin. Every holding and notification a pass
 * owes is reserved before anything joins, so a pass that runs out of memory changes nothing.
 *
 * @return WEIVEL_STATUS_OK when it passes; WEIVEL_STATUS_REFUSED when the plugin provided less or more than it
 * declares (WEIVEL_REFUSAL_ACTIVATION_MISMATCH), for a stale plugin, or for one not open
 * (WEIVEL_REFUSAL_PLUGIN_STATE), each queuing a refusal; WEIVEL_STATUS_MALFORMED for a NULL core;
 * WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_verify_activation(weivel_core *core, weivel_handle plugin,
                                                  uint32_t *pending_events);

/** One implementation of a capability: the handle its provider was answered, and the providing plugin. */
typedef struct weivel_capability_implementation {
    uint32_t size;
    weivel_handle implementation;
    weivel_handle provider;
} weivel_capability_implementation;

/**
 * The implementations of a capability, filled into a struct the caller owns. `items` holds `count` size-prefixed
 * elements, the kind that may grow (see Answers at the head of this file), stepped through by the first one's
 * `size`, in `storage`, private to the core, until weivel_capability_list_release.
 */
typedef struct weivel_capability_list {
    uint32_t size;
    const weivel_capability_implementation *items;
    uint32_t count;
    void *storage;
} weivel_capability_list;

/**
 * Answers every verified implementation of `id`, in the order their plugins' activations were verified, which is
 * not the order they were provided.
 *
 * Reach is checked first: the plugin must list `id` among the capabilities it consumes or provides, else it is
 * answered an empty list and refused (WEIVEL_REFUSAL_UNDECLARED), reported as a WEIVEL_EVENT_REACH_REFUSED of kind
 * WEIVEL_REACH_CAPABILITY once per plugin and id. Then a plugin no longer running is let through. The host
 * (WEIVEL_HOST) reaches every id.
 *
 * The holder rule is the services' rule (weivel_unregister): a plugin holds each implementation this call or a
 * capability watch hearing it provided hands it, once however often, until the implementation is withdrawn or the
 * plugin is released or quarantined. A withdrawal, when its provider is quarantined, released or forgotten, revokes
 * it to every holder by one WEIVEL_EVENT_REVOKED each, before any watch hears it withdrawn; a plugin that holds and
 * watches gets both. A plugin released or quarantined has what it holds revoked to it in the same call, and a
 * released plugin's late `all` holds again until the implementation goes or the plugin is forgotten. When a holding
 * cannot be recorded, the call answers WEIVEL_STATUS_OUT_OF_MEMORY and hands nothing over.
 *
 * @param list the caller's struct with `size` set and nothing held; on WEIVEL_STATUS_OK it must be passed to
 * weivel_capability_list_release.
 * @return WEIVEL_STATUS_OK, also with no implementation; WEIVEL_STATUS_REFUSED for an undeclared id, a stale plugin
 * or one never opened, the list left empty; WEIVEL_STATUS_MALFORMED for a NULL core, id or list, an empty id, or a
 * list below its first shape or still holding one; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_all_capabilities(weivel_core *core, weivel_handle plugin, const char *id,
                                                 weivel_capability_list *list, uint32_t *pending_events);

/**
 * Frees what a capability list carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty list or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the list's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_capability_list_release(weivel_capability_list *list);

/**
 * Watches `id`: each later change to its verified implementations is a WEIVEL_EVENT_CAPABILITY, and nothing from
 * before the watch is replayed. Hearing an implementation provided hands it over, so the plugin holds it under the
 * holder rule of weivel_all_capabilities, and is revoked it before the watch hears it withdrawn. The watch lasts
 * until weivel_unregister, or until the plugin is released or quarantined, which first withdraws what the plugin
 * itself provided, so a plugin watching its own capability hears its own implementation leave.
 *
 * Reach is checked as weivel_all_capabilities checks it, and a refused watch is inert; the host (WEIVEL_HOST) watches
 * unfenced. A late watch, from a plugin no longer running, is inert and logged only, never reported.
 *
 * @param watch receives the watch handle. Required; 0 on every answer but OK.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for an undeclared id, a late watch, a stale plugin or one never
 * opened; WEIVEL_STATUS_MALFORMED for a NULL core, id or watch or an empty id; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_watch_capability(weivel_core *core, weivel_handle plugin, const char *id,
                                                 weivel_handle *watch, uint32_t *pending_events);

/** Where a plugin is in its life, as the ledger shows it. */
typedef enum weivel_plugin_status {
    /** Registered and never opened. */
    WEIVEL_PLUGIN_STATUS_REGISTERED = 1,
    /** Open, so its calls are on time. */
    WEIVEL_PLUGIN_STATUS_ACTIVE = 2,
    /** Quarantined, until it is opened again. */
    WEIVEL_PLUGIN_STATUS_QUARANTINED = 3,
    /** Released, until it is forgotten. */
    WEIVEL_PLUGIN_STATUS_RELEASED = 4
} weivel_plugin_status;

/**
 * One plugin's row of the ledger: what it declared and every relationship that passed through the core for it.
 *
 * The relationships describe one activation: opening the plugin starts them afresh, and releasing or quarantining
 * it keeps them, so a stopped plugin's row still says what it did. Each list holds an id once, in the order the
 * relationship was first made, and is never shortened when the relationship ends.
 *
 * - `capabilities_declared`: the capabilities its manifest provides; `permissions`: those its manifest asks for.
 * - `services_provided`: each service id it registered; `capabilities_provided`: each capability it provided,
 *   verified or not.
 * - `services_consumed`: each service id it asked for by a get, a want or a watch its reach allowed, answered or
 *   not; `capabilities_consumed`, appended after `storage`, likewise each capability id by an `all` or a watch.
 *
 * Every text and list lives in `storage`, private to the core, until weivel_ledger_row_release; in a row a
 * weivel_ledger_snapshot carries, `storage` is NULL and the snapshot owns everything.
 */
typedef struct weivel_ledger_row {
    uint32_t size;
    weivel_handle plugin;
    const char *import_name;
    const char *identity;
    /** A weivel_plugin_status. */
    uint32_t status;
    /** 1 when weivel_verify_activation has passed the plugin since it was last opened, else 0. */
    uint32_t verified;
    /**
     * For a quarantined plugin, why: the last reason weivel_quarantine_plugin was given since the plugin was last
     * opened, else the refusal weivel_verify_activation answered its activation since then. Otherwise, and for a
     * plugin in any other status, code NONE with NULL texts.
     */
    weivel_refusal refusal;
    weivel_strings capabilities_declared;
    weivel_strings permissions;
    weivel_strings services_provided;
    weivel_strings capabilities_provided;
    weivel_strings services_consumed;
    void *storage;
    /**
     * Each capability id it asked for by an `all` or a watch its reach allowed, answered or not, as
     * `services_consumed` is for services. Appended after `storage`.
     */
    weivel_strings capabilities_consumed;
    /**
     * The subject of `refusal` (see weivel_refusal): the import name for a refused activation, NULL for a reason
     * weivel_quarantine_plugin was given, which names none. Appended after `capabilities_consumed`.
     */
    const char *subject;
    /** 1 when `refusal.fix` is non-empty, else 0. Appended after `subject`. */
    uint32_t has_fix;
} weivel_ledger_row;

/**
 * A refusal of reach reported for a plugin: it reached for the `kind` (a weivel_reach_kind) named `id`, and `code`
 * says why: WEIVEL_REFUSAL_UNDECLARED when it does not declare it, WEIVEL_REFUSAL_WRONG_PROVIDER when it binds a
 * service to one provider and another owner holds it.
 */
typedef struct weivel_declaration_refusal {
    uint32_t size;
    uint32_t kind;
    weivel_handle plugin;
    const char *id;
    /** A weivel_refusal_code. Appended after `id`. */
    uint32_t code;
    /** The refusal's subject, the same text as `id`. Appended after `code`. */
    const char *subject;
    /** 1 when the fix its reach event carried is non-empty, else 0. Appended after `subject`. */
    uint32_t has_fix;
} weivel_declaration_refusal;

/**
 * The ledger as it stood at one moment, filled into a struct the caller owns.
 *
 * - `rows`: one per plugin registered and not forgotten, in the order they were registered, as size-prefixed
 *   elements, the kind that may grow (see Answers at the head of this file), stepped through by the first one's
 *   `size`.
 * - `refusals`: every refusal of reach reported for those plugins, in the order reported, each once per plugin,
 *   kind and id, likewise size-prefixed. A forgotten plugin's refusals go with its row.
 *
 * Everything lives in `storage`, private to the core, until weivel_ledger_snapshot_release; an empty array is NULL with
 * count 0.
 */
typedef struct weivel_ledger_snapshot {
    uint32_t size;
    const weivel_ledger_row *rows;
    uint32_t row_count;
    const weivel_declaration_refusal *refusals;
    uint32_t refusal_count;
    void *storage;
} weivel_ledger_snapshot;

/**
 * Answers the whole ledger. Nothing in it changes when the core does afterwards.
 *
 * @param snapshot the caller's struct with `size` set and nothing held; on WEIVEL_STATUS_OK it must be passed to
 * weivel_ledger_snapshot_release.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_MALFORMED for a NULL core or snapshot, a snapshot below its first shape or
 * still holding one, queuing a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_ledger(weivel_core *core, weivel_ledger_snapshot *snapshot, uint32_t *pending_events);

/**
 * Frees what a snapshot carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty snapshot or an all-zero one (size 0);
 * WEIVEL_STATUS_MALFORMED for NULL or a size between 1 and the snapshot's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_ledger_snapshot_release(weivel_ledger_snapshot *snapshot);

/**
 * Answers one plugin's row, as weivel_ledger would show it.
 *
 * @param row the caller's struct with `size` set and nothing held; on WEIVEL_STATUS_OK it must be passed to
 * weivel_ledger_row_release.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a handle that names no plugin, a forgotten one included
 * (WEIVEL_REFUSAL_STALE_HANDLE), queuing a refusal; WEIVEL_STATUS_MALFORMED for a NULL core or row, or a row below
 * its first shape or still holding one; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_ledger_entry(weivel_core *core, weivel_handle plugin, weivel_ledger_row *row,
                                             uint32_t *pending_events);

/**
 * Frees what a row weivel_ledger_entry answered carries and leaves it empty.
 *
 * @return WEIVEL_STATUS_OK, also as a no-op for an empty row or an all-zero one (size 0); WEIVEL_STATUS_MALFORMED
 * for NULL or a size between 1 and the row's first shape.
 * Threading: any thread; needs no core.
 */
WEIVEL_API weivel_status weivel_ledger_row_release(weivel_ledger_row *row);

#ifdef __cplusplus
}
#endif

#endif

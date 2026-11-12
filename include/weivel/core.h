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
 *   WEIVEL_EVENT_REFUSAL saying what was wrong.
 * - Handles. Anything the core tracks is a weivel_handle: 32 bits of slot and 32 bits of generation, never 0, valid
 *   only for the core that issued it. A handle whose object is gone is refused, never mistaken for a newer one.
 * - Events. The core never calls out. Every change lands in a queue the caller drains with weivel_next_event after
 *   the call returns; every call that can queue an event reports how many wait.
 * - Strings. Strings handed in are copied before the call returns. Strings handed out are UTF-8, NUL-terminated,
 *   and belong to the struct that carries them until its release call.
 * - Time. The core owns no clock. Every timestamp is a caller-supplied int64_t count of milliseconds on a
 *   monotonic scale the caller chooses. The core answers its next deadline and acts on time only in
 *   weivel_advance.
 * - Threads. Every call taking a core may be made from any thread; one lock inside the core serialises them and
 *   is never held while anything outside the core runs.
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

/** Why the core refused something. */
typedef enum weivel_refusal_code {
    WEIVEL_REFUSAL_NONE = 0,
    /** A call was malformed; detail names the call and what was wrong with it. */
    WEIVEL_REFUSAL_MALFORMED_CALL = 1,
    /** A handle named something that no longer exists, or never did. */
    WEIVEL_REFUSAL_STALE_HANDLE = 2,
    /** A manifest's own shape is wrong (no import name, a NULL in a list, an id listed twice); detail says where. */
    WEIVEL_REFUSAL_MALFORMED_MANIFEST = 3,
    /** This host cannot load the manifest's plugin; the answer's axis says which check refused it. */
    WEIVEL_REFUSAL_INCOMPATIBLE = 4,
    /** A plugin with this import name is already registered on this core; detail names it. */
    WEIVEL_REFUSAL_NAME_TAKEN = 5,
    /** A plugin needs something mandatory that nothing in its activation set provides; detail names the need. */
    WEIVEL_REFUSAL_UNMET_NEED = 6,
    /** A plugin was listed more than once in one activation set; its later listings are refused. */
    WEIVEL_REFUSAL_REPEATED_PLUGIN = 7,
    /** A plugin's lifecycle does not allow the call: it was never opened, is open already, or was released. */
    WEIVEL_REFUSAL_PLUGIN_STATE = 8,
    /** Another plugin that is not released already holds the identity a plugin is opened with. */
    WEIVEL_REFUSAL_IDENTITY_TAKEN = 9,
    /** A plugin registered a service id outside its namespace that is not well known. */
    WEIVEL_REFUSAL_OUTSIDE_NAMESPACE = 10,
    /** A plugin registered a service id another owner already holds. */
    WEIVEL_REFUSAL_SERVICE_TAKEN = 11,
    /** A plugin reached for a service it neither consumes nor provides in its manifest. */
    WEIVEL_REFUSAL_UNDECLARED = 12,
    /** A plugin reached for a service it binds to one provider while another owner provides it. */
    WEIVEL_REFUSAL_WRONG_PROVIDER = 13,
    /** A plugin was stopped while it waited, or asked to wait after it stopped. */
    WEIVEL_REFUSAL_STOPPED = 14,
    /** A want's deadline passed before a service it accepts was registered. */
    WEIVEL_REFUSAL_TIMED_OUT = 15
} weivel_refusal_code;

/**
 * A refusal, as a value. `detail` says what happened and `fix` what would make it succeed, both English UTF-8.
 * Both strings belong to the event carrying the refusal until weivel_event_release.
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

/** What an event reports. New kinds are only appended; a caller ignores, and still releases, a kind it does not know. */
typedef enum weivel_event_kind {
    /** No event: what weivel_next_event leaves when the queue is empty, and what release leaves behind. */
    WEIVEL_EVENT_NONE = 0,
    /** A diagnostic; the payload is `data.log`. */
    WEIVEL_EVENT_LOG = 1,
    /** A refusal; the payload is `data.refusal`. */
    WEIVEL_EVENT_REFUSAL = 2,
    /** A want settled, with a service or with a refusal, exactly once; the payload is `data.want`. */
    WEIVEL_EVENT_WANT_SETTLED = 3,
    /** A watched service was registered or unregistered; the payload is `data.watch`. */
    WEIVEL_EVENT_WATCH = 4,
    /** A service a plugin holds was withdrawn, so the plugin must stop using it; the payload is `data.revoked`. */
    WEIVEL_EVENT_REVOKED = 5,
    /** A plugin reached for what it may not, reported once per plugin, kind and id; the payload is `data.reach`. */
    WEIVEL_EVENT_REACH_REFUSED = 6
} weivel_event_kind;

/**
 * How a want settled. `implementation` is the registration handle of the service it settled with, or 0 when it was
 * rejected, and then `refusal` says why (WEIVEL_REFUSAL_UNDECLARED, WEIVEL_REFUSAL_WRONG_PROVIDER,
 * WEIVEL_REFUSAL_STOPPED or WEIVEL_REFUSAL_TIMED_OUT); a settled want's `refusal` is code NONE with NULL texts.
 */
typedef struct weivel_want_event {
    weivel_handle want;
    weivel_handle plugin;
    weivel_handle implementation;
    weivel_refusal refusal;
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

/** A withdrawal: `plugin` held `implementation`, a registration handle that is gone. */
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
} weivel_reach_refusal;

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
} weivel_manifest;

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
} weivel_core_options;

/**
 * Creates a core.
 *
 * @param options may be NULL for every default; read only during the call.
 * @param core receives the new core, owned by the caller until weivel_core_destroy. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK, or WEIVEL_STATUS_MALFORMED (no `core`, options smaller than their first shape, an
 * initial_event_capacity above its maximum, or an `applications` or `well_known_services` list that cannot be
 * read) or
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
 * Answers the earliest deadline the core is waiting on, so the caller can arm its own timer for it.
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
 * import name namespaces a plugin's services, queuing one with code WEIVEL_REFUSAL_NAME_TAKEN; WEIVEL_STATUS_MALFORMED for a NULL core, manifest or plugin or a
 * manifest smaller than its first shape, queuing a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
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
 *
 * @implNote The element structs of these arrays never grow, since a caller indexes them at its own compiled stride;
 * anything a later version adds to a plan arrives as a new array appended to this struct.
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
 * The owner of the host's own services: a reserved handle no plugin is ever issued. A service registered under it
 * is exempt from namespacing, owned by no plugin so no release or quarantine takes it away, and registering its id
 * again replaces it.
 */
#define WEIVEL_HOST ((weivel_handle)UINT64_MAX)

/** How a plugin is opened. Zero, or a field beyond `size`, means the default. */
typedef struct weivel_plugin_options {
    uint32_t size;
    /**
     * Who the plugin is, opaque, supplied by whatever loaded it: the name a consumer's `provider` binds to. Copied.
     * NULL means its import name.
     */
    const char *identity;
    /** The caller's own token for the plugin, carried through uninspected and answered by weivel_plugin_payload. */
    uint64_t payload;
} weivel_plugin_options;

/**
 * Opens a registered plugin, so it may register, get, want and watch services. Opening a quarantined plugin again
 * lifts its fence, under the identity given now.
 *
 * @param plugin a handle weivel_register_manifest answered.
 * @param options may be NULL for every default; read only during the call.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle (WEIVEL_REFUSAL_STALE_HANDLE), a plugin open
 * already or released (WEIVEL_REFUSAL_PLUGIN_STATE), or an identity another plugin that is not released holds
 * (WEIVEL_REFUSAL_IDENTITY_TAKEN), each queuing a refusal; WEIVEL_STATUS_MALFORMED for a NULL core, options below
 * their first shape or an empty identity; WEIVEL_STATUS_OUT_OF_MEMORY.
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
 * Releases a plugin, in one call: its watches stop, everything it registered is unregistered (each holder told by a
 * WEIVEL_EVENT_REVOKED and each watch by a WEIVEL_EVENT_WATCH), every want it still waits on is rejected with
 * WEIVEL_REFUSAL_STOPPED, and every service it holds is revoked to it. Its import name and identity are free again
 * at once, so a plugin of the same name may be registered.
 *
 * The handle stays valid, so a call the released plugin still makes is late: a `get` is let through, a `want` is
 * rejected, a `watch` or a registration is refused with a log only. Its record is kept until the core is destroyed.
 * Releasing a released plugin does nothing.
 *
 * @return WEIVEL_STATUS_OK; WEIVEL_STATUS_REFUSED for a stale handle; WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_release_plugin(weivel_core *core, weivel_handle plugin, uint32_t *pending_events);

/**
 * Quarantines a plugin: everything weivel_release_plugin takes from it goes in the same one call, but it keeps its
 * import name and identity, and opening it again lifts the fence. Its calls meanwhile are late, as a released
 * plugin's are.
 *
 * @return WEIVEL_STATUS_OK, also for a plugin quarantined already; WEIVEL_STATUS_REFUSED for a stale handle or a
 * released plugin (WEIVEL_REFUSAL_PLUGIN_STATE); WEIVEL_STATUS_MALFORMED for a NULL core.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_quarantine_plugin(weivel_core *core, weivel_handle plugin,
                                                  uint32_t *pending_events);

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
 * Ends what a handle stands for: a registration is unregistered, a watch stops, and a want still waiting is
 * withdrawn without settling.
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

/**
 * Answers the service registered under `id` now, as its registration handle, for a plugin that may reach it.
 *
 * Reach is checked in this order: the plugin must list `id` among the services it consumes or provides
 * (WEIVEL_REFUSAL_UNDECLARED); then, when it binds `id` to a provider and another owner holds it, it is refused
 * (WEIVEL_REFUSAL_WRONG_PROVIDER); then a plugin no longer running is late, and a late `get` is let through. A
 * refusal is reported, a WEIVEL_EVENT_REACH_REFUSED, once per plugin and id. The plugin then holds what it got.
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
    /** Whether the want has a deadline, a weivel_choice. Default NO, so it waits for as long as it takes. */
    uint32_t timed;
    /** The caller's time now, on its own monotonic scale in milliseconds; read only when `timed` is YES. */
    int64_t now;
    /** How long after `now` the want gives up, at least 0; read only when `timed` is YES. */
    int64_t timeout_millis;
} weivel_want_options;

/**
 * Asks for the service registered under `id`, now or whenever it arrives. Every want settles at most once, by one
 * WEIVEL_EVENT_WANT_SETTLED carrying the want handle: with the service when one it accepts is registered, which the
 * plugin then holds, or with a refusal. Without a deadline it never settles until then. With one it settles
 * either way exactly once: weivel_advance at or past `now + timeout_millis` rejects it with
 * WEIVEL_REFUSAL_TIMED_OUT, and weivel_next_deadline reports the earliest such moment.
 *
 * Reach is checked as weivel_get_service checks it, and a refusal rejects the want at once. A late want, from a
 * plugin no longer running, is rejected with WEIVEL_REFUSAL_STOPPED and logged. A want the plugin still waits on
 * when it is released or quarantined is rejected the same way.
 *
 * @param options may be NULL for no deadline; read only during the call.
 * @param want receives the want handle, which names the want until it settles. Required.
 * @return WEIVEL_STATUS_OK with a want, including one rejected at once; WEIVEL_STATUS_REFUSED for a stale plugin
 * or one never opened; WEIVEL_STATUS_MALFORMED for a NULL core, id or want, an empty id, options below their first
 * shape, `timed` no weivel_choice or a negative timeout; WEIVEL_STATUS_OUT_OF_MEMORY.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_want_service(weivel_core *core, weivel_handle plugin, const char *id,
                                             const weivel_want_options *options, weivel_handle *want,
                                             uint32_t *pending_events);

/**
 * Watches `id`: each later registration and unregistration of it that the plugin accepts is a WEIVEL_EVENT_WATCH,
 * and nothing registered before the watch is replayed. A watch bound to another provider skips its registrations
 * without a report. The watch lasts until weivel_unregister or until the plugin is released or quarantined.
 *
 * Reach is checked as weivel_get_service checks it, and a refused watch is inert. A late watch is inert and logged
 * only, never reported.
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

#ifdef __cplusplus
}
#endif

#endif

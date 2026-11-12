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
    /** The core declined the request; a WEIVEL_EVENT_REFUSAL says why. */
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
    WEIVEL_REFUSAL_INCOMPATIBLE = 4
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
    WEIVEL_EVENT_REFUSAL = 2
} weivel_event_kind;

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
    /** The highest api level this host implements. Default 0, so a plugin declaring any api above 0 is refused. */
    uint32_t api;
    /** The applications this host hosts, opaque, copied by weivel_core_create; empty means every application. */
    weivel_strings applications;
} weivel_core_options;

/**
 * Creates a core.
 *
 * @param options may be NULL for every default; read only during the call.
 * @param core receives the new core, owned by the caller until weivel_core_destroy. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK, or WEIVEL_STATUS_MALFORMED (no `core`, options smaller than their first shape, an
 * initial_event_capacity above its maximum, or an `applications` list holding a NULL) or
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
 * with code WEIVEL_REFUSAL_MALFORMED_MANIFEST; WEIVEL_STATUS_MALFORMED for a NULL core, manifest or plugin or a
 * manifest smaller than its first shape, queuing a refusal when the core is given; WEIVEL_STATUS_OUT_OF_MEMORY.
 * `*plugin` is 0 on every answer but OK.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_register_manifest(weivel_core *core, const weivel_manifest *manifest,
                                                  weivel_handle *plugin, uint32_t *pending_events);

#ifdef __cplusplus
}
#endif

#endif

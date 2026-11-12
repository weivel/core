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

#include <stddef.h>
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
    WEIVEL_REFUSAL_STALE_HANDLE = 2
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
        uint64_t reserved[16];
    } data;
} weivel_event;

/** Options for weivel_core_create. Zero, or a field beyond `size`, means the default. */
typedef struct weivel_core_options {
    uint32_t size;
    /** How many events the queue holds before it first grows. Default 16. */
    uint32_t initial_event_capacity;
    /** The least level of log event the core queues, a weivel_log_level. Default WEIVEL_LOG_WARNING. */
    uint32_t minimum_log_level;
} weivel_core_options;

/**
 * Creates a core.
 *
 * @param options may be NULL for every default; read only during the call.
 * @param core receives the new core, owned by the caller until weivel_core_destroy. Required.
 * @param pending_events receives how many events wait; may be NULL.
 * @return WEIVEL_STATUS_OK, WEIVEL_STATUS_MALFORMED, or WEIVEL_STATUS_OUT_OF_MEMORY with `*core` set to NULL.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_core_create(const weivel_core_options *options, weivel_core **core,
                                            uint32_t *pending_events);

/**
 * Destroys a core and every event still queued in it. Every handle it issued becomes meaningless. Events already
 * drained stay valid until their own release.
 *
 * Threading: no other call on this core may be in flight or follow.
 */
WEIVEL_API weivel_status weivel_core_destroy(weivel_core *core);

/**
 * Moves the oldest queued event into `event`, which the caller owns. Events come out once each, in the order they
 * were queued.
 *
 * @param event the caller's struct with `size` set; on WEIVEL_STATUS_OK it holds the event and must be passed to
 * weivel_event_release; on WEIVEL_STATUS_NONE its kind is WEIVEL_EVENT_NONE and it holds nothing to release.
 * @param pending_events receives how many events still wait; may be NULL.
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_next_event(weivel_core *core, weivel_event *event, uint32_t *pending_events);

/**
 * Frees what an event carries and leaves it WEIVEL_EVENT_NONE. Releasing a NONE event does nothing.
 *
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
 * Threading: any thread.
 */
WEIVEL_API weivel_status weivel_advance(weivel_core *core, int64_t now, uint32_t *pending_events);

#ifdef __cplusplus
}
#endif

#endif

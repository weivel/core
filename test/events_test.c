#include <stdbool.h>
#include <stdlib.h>
#include <weivel/core.h>

#include "check.h"
#include "events.h"

static const weivel_deadline_event at_five = {5, 1};

static void release_popped(weivel_events *events, uint32_t expected_kind) {
    weivel_event event;
    CHECK(weivel_events_pop(events, &event));
    CHECK(event.kind == expected_kind);
    free(event.storage);
}

/** An undo after an announcement takes back its own event, never the deadline the caller arms from. */
static void dropping_the_newest_keeps_a_waiting_deadline(void) {
    weivel_events *events = weivel_events_create(0);
    CHECK(weivel_events_push_log(events, WEIVEL_LOG_INFO, "undone") == WEIVEL_STATUS_OK);
    weivel_events_announce_deadline(events, &at_five);

    weivel_events_drop_newest(events);

    CHECK(weivel_events_pending(events) == 1);
    weivel_event event;
    CHECK(weivel_events_pop(events, &event));
    CHECK(event.kind == WEIVEL_EVENT_DEADLINE);
    CHECK(event.data.deadline.set == 1 && event.data.deadline.at == 5);
    weivel_events_destroy(events);
}

static void dropping_the_newest_takes_the_event_after_a_deadline(void) {
    weivel_events *events = weivel_events_create(0);
    weivel_events_announce_deadline(events, &at_five);
    CHECK(weivel_events_push_log(events, WEIVEL_LOG_INFO, "undone") == WEIVEL_STATUS_OK);

    weivel_events_drop_newest(events);

    CHECK(weivel_events_pending(events) == 1);
    release_popped(events, WEIVEL_EVENT_DEADLINE);
    weivel_events_destroy(events);
}

static void dropping_the_newest_keeps_the_older_events_in_order(void) {
    weivel_events *events = weivel_events_create(0);
    CHECK(weivel_events_push_log(events, WEIVEL_LOG_INFO, "kept") == WEIVEL_STATUS_OK);
    CHECK(weivel_events_push_log(events, WEIVEL_LOG_INFO, "undone") == WEIVEL_STATUS_OK);
    weivel_events_announce_deadline(events, &at_five);

    weivel_events_drop_newest(events);

    CHECK(weivel_events_pending(events) == 2);
    weivel_event event;
    CHECK(weivel_events_pop(events, &event));
    CHECK(event.kind == WEIVEL_EVENT_LOG);
    check_text(event.data.log.text, "kept", "the older log");
    free(event.storage);
    release_popped(events, WEIVEL_EVENT_DEADLINE);
    weivel_events_destroy(events);
}

int main(void) {
    dropping_the_newest_keeps_a_waiting_deadline();
    dropping_the_newest_takes_the_event_after_a_deadline();
    dropping_the_newest_keeps_the_older_events_in_order();
    return check_summary("events_test");
}

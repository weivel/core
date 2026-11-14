#include <stdio.h>

#include "weivel/core.h"

/**
 * @file
 * Prints the layout of every public struct core.h declares, as this compiler lays it out, so a language that declares
 * the same layouts by hand compares its own against the C compiler's rather than against a guess.
 *
 * Every line is plain text, fields separated by one space, numbers in decimal bytes:
 *
 *     <struct> <field> <offset> <size>
 *     <struct> sizeof <size>
 *
 * A struct's field lines come in declaration order and are followed by its sizeof line. A field of struct type is one
 * line; that struct is listed by its own name. A member of an unnamed union is named after the union's field, as in
 * `weivel_event data.want`, and sits at the union's offset, after a line for the union's field itself. An opaque
 * struct has no layout and is not listed.
 *
 * An array field is one line carrying the array's total size and no element count, as `weivel_event data.reserved`
 * does, so a consumer takes the element type and count from its own declaration. Lines end in CRLF on Windows, where
 * stdout is a text stream, and in LF elsewhere, so a consumer strips a trailing carriage return before splitting.
 */

static void print_field(const char *type, const char *field, const void *instance, const void *member,
                        size_t size) {
    printf("%s %s %zu %zu\n", type, field, (size_t)((const char *)member - (const char *)instance), size);
}

static void print_size(const char *type, size_t size) {
    printf("%s sizeof %zu\n", type, size);
}

#define FIELD(type, member)                                                                  \
    do {                                                                                     \
        static type instance;                                                                \
        print_field(#type, #member, &instance, &instance.member, sizeof instance.member);    \
    } while (0)

#define SIZE(type) print_size(#type, sizeof(type))

static void print_event_layouts(void) {
    FIELD(weivel_refusal, code);
    FIELD(weivel_refusal, detail);
    FIELD(weivel_refusal, fix);
    SIZE(weivel_refusal);
    FIELD(weivel_log_event, level);
    FIELD(weivel_log_event, text);
    SIZE(weivel_log_event);
    FIELD(weivel_want_event, want);
    FIELD(weivel_want_event, plugin);
    FIELD(weivel_want_event, implementation);
    FIELD(weivel_want_event, refusal);
    FIELD(weivel_want_event, degraded);
    FIELD(weivel_want_event, subject);
    FIELD(weivel_want_event, has_fix);
    SIZE(weivel_want_event);
    FIELD(weivel_watch_event, watch);
    FIELD(weivel_watch_event, plugin);
    FIELD(weivel_watch_event, implementation);
    FIELD(weivel_watch_event, change);
    SIZE(weivel_watch_event);
    FIELD(weivel_revoked_event, plugin);
    FIELD(weivel_revoked_event, implementation);
    SIZE(weivel_revoked_event);
    FIELD(weivel_reach_refusal, kind);
    FIELD(weivel_reach_refusal, plugin);
    FIELD(weivel_reach_refusal, id);
    FIELD(weivel_reach_refusal, refusal);
    FIELD(weivel_reach_refusal, subject);
    FIELD(weivel_reach_refusal, has_fix);
    SIZE(weivel_reach_refusal);
    FIELD(weivel_refusal_event, refusal);
    FIELD(weivel_refusal_event, subject);
    FIELD(weivel_refusal_event, has_fix);
    SIZE(weivel_refusal_event);
    FIELD(weivel_number, size);
    FIELD(weivel_number, name);
    FIELD(weivel_number, value);
    SIZE(weivel_number);
    FIELD(weivel_numbers, items);
    FIELD(weivel_numbers, count);
    SIZE(weivel_numbers);
    FIELD(weivel_capacity_event, want);
    FIELD(weivel_capacity_event, plugin);
    FIELD(weivel_capacity_event, registration);
    FIELD(weivel_capacity_event, request);
    FIELD(weivel_capacity_event, question);
    FIELD(weivel_capacity_event, constraints);
    SIZE(weivel_capacity_event);
    FIELD(weivel_capability_event, watch);
    FIELD(weivel_capability_event, plugin);
    FIELD(weivel_capability_event, implementation);
    FIELD(weivel_capability_event, provider);
    FIELD(weivel_capability_event, change);
    SIZE(weivel_capability_event);
    FIELD(weivel_deadline_event, at);
    FIELD(weivel_deadline_event, set);
    SIZE(weivel_deadline_event);
    FIELD(weivel_event, size);
    FIELD(weivel_event, kind);
    FIELD(weivel_event, storage);
    FIELD(weivel_event, data);
    FIELD(weivel_event, data.log);
    FIELD(weivel_event, data.refusal);
    FIELD(weivel_event, data.want);
    FIELD(weivel_event, data.watch);
    FIELD(weivel_event, data.revoked);
    FIELD(weivel_event, data.reach);
    FIELD(weivel_event, data.capacity);
    FIELD(weivel_event, data.capability);
    FIELD(weivel_event, data.refused);
    FIELD(weivel_event, data.deadline);
    FIELD(weivel_event, data.reserved);
    SIZE(weivel_event);
}

static void print_manifest_layouts(void) {
    FIELD(weivel_strings, items);
    FIELD(weivel_strings, count);
    SIZE(weivel_strings);
    FIELD(weivel_service_offer, size);
    FIELD(weivel_service_offer, id);
    FIELD(weivel_service_offer, attributes);
    SIZE(weivel_service_offer);
    FIELD(weivel_service_offers, items);
    FIELD(weivel_service_offers, count);
    SIZE(weivel_service_offers);
    FIELD(weivel_service_want, size);
    FIELD(weivel_service_want, id);
    FIELD(weivel_service_want, provider);
    FIELD(weivel_service_want, optional);
    FIELD(weivel_service_want, degradable);
    FIELD(weivel_service_want, constraints);
    SIZE(weivel_service_want);
    FIELD(weivel_service_wants, items);
    FIELD(weivel_service_wants, count);
    SIZE(weivel_service_wants);
    FIELD(weivel_capability_declarations, provides);
    FIELD(weivel_capability_declarations, consumes);
    SIZE(weivel_capability_declarations);
    FIELD(weivel_service_declarations, provides);
    FIELD(weivel_service_declarations, consumes);
    SIZE(weivel_service_declarations);
    FIELD(weivel_manifest, size);
    FIELD(weivel_manifest, import_name);
    FIELD(weivel_manifest, api);
    FIELD(weivel_manifest, entry);
    FIELD(weivel_manifest, targets);
    FIELD(weivel_manifest, capabilities);
    FIELD(weivel_manifest, services);
    FIELD(weivel_manifest, permissions);
    FIELD(weivel_manifest, identity);
    SIZE(weivel_manifest);
}

static void print_option_layouts(void) {
    FIELD(weivel_constraint_entry, size);
    FIELD(weivel_constraint_entry, constraint_name);
    FIELD(weivel_constraint_entry, attribute_name);
    FIELD(weivel_constraint_entry, bound);
    SIZE(weivel_constraint_entry);
    FIELD(weivel_constraint_vocabulary, items);
    FIELD(weivel_constraint_vocabulary, count);
    SIZE(weivel_constraint_vocabulary);
    FIELD(weivel_core_options, size);
    FIELD(weivel_core_options, initial_event_capacity);
    FIELD(weivel_core_options, minimum_log_level);
    FIELD(weivel_core_options, api);
    FIELD(weivel_core_options, applications);
    FIELD(weivel_core_options, well_known_services);
    FIELD(weivel_core_options, constraint_vocabulary);
    SIZE(weivel_core_options);
}

static void print_activation_layouts(void) {
    FIELD(weivel_support, size);
    FIELD(weivel_support, supported);
    FIELD(weivel_support, axis);
    FIELD(weivel_support, refusal);
    FIELD(weivel_support, storage);
    FIELD(weivel_support, subject);
    FIELD(weivel_support, has_fix);
    SIZE(weivel_support);
    FIELD(weivel_edge, provider);
    FIELD(weivel_edge, consumer);
    SIZE(weivel_edge);
    FIELD(weivel_cycle, members);
    FIELD(weivel_cycle, count);
    SIZE(weivel_cycle);
    FIELD(weivel_plugin_refusal, plugin);
    FIELD(weivel_plugin_refusal, refusal);
    SIZE(weivel_plugin_refusal);
    FIELD(weivel_refusal_subject, subject);
    FIELD(weivel_refusal_subject, has_fix);
    SIZE(weivel_refusal_subject);
    FIELD(weivel_activation_plan, size);
    FIELD(weivel_activation_plan, order);
    FIELD(weivel_activation_plan, order_count);
    FIELD(weivel_activation_plan, edges);
    FIELD(weivel_activation_plan, edge_count);
    FIELD(weivel_activation_plan, cycles);
    FIELD(weivel_activation_plan, cycle_count);
    FIELD(weivel_activation_plan, refusals);
    FIELD(weivel_activation_plan, refusal_count);
    FIELD(weivel_activation_plan, storage);
    FIELD(weivel_activation_plan, refusal_subjects);
    SIZE(weivel_activation_plan);
}

static void print_call_layouts(void) {
    FIELD(weivel_plugin_options, size);
    FIELD(weivel_plugin_options, payload);
    SIZE(weivel_plugin_options);
    FIELD(weivel_quarantine_reason, size);
    FIELD(weivel_quarantine_reason, code);
    FIELD(weivel_quarantine_reason, detail);
    FIELD(weivel_quarantine_reason, fix);
    SIZE(weivel_quarantine_reason);
    FIELD(weivel_want_options, size);
    FIELD(weivel_want_options, timed);
    FIELD(weivel_want_options, now);
    FIELD(weivel_want_options, timeout_millis);
    FIELD(weivel_want_options, budgeted);
    FIELD(weivel_want_options, budget_millis);
    FIELD(weivel_want_options, poll_millis);
    SIZE(weivel_want_options);
    FIELD(weivel_capacity, size);
    FIELD(weivel_capacity, request);
    FIELD(weivel_capacity, attributes);
    FIELD(weivel_capacity, serves);
    SIZE(weivel_capacity);
}

static void print_answer_layouts(void) {
    FIELD(weivel_service_list, size);
    FIELD(weivel_service_list, ids);
    FIELD(weivel_service_list, storage);
    SIZE(weivel_service_list);
    FIELD(weivel_capability_implementation, size);
    FIELD(weivel_capability_implementation, implementation);
    FIELD(weivel_capability_implementation, provider);
    SIZE(weivel_capability_implementation);
    FIELD(weivel_capability_list, size);
    FIELD(weivel_capability_list, items);
    FIELD(weivel_capability_list, count);
    FIELD(weivel_capability_list, storage);
    SIZE(weivel_capability_list);
    FIELD(weivel_ledger_row, size);
    FIELD(weivel_ledger_row, plugin);
    FIELD(weivel_ledger_row, import_name);
    FIELD(weivel_ledger_row, identity);
    FIELD(weivel_ledger_row, status);
    FIELD(weivel_ledger_row, verified);
    FIELD(weivel_ledger_row, refusal);
    FIELD(weivel_ledger_row, capabilities_declared);
    FIELD(weivel_ledger_row, permissions);
    FIELD(weivel_ledger_row, services_provided);
    FIELD(weivel_ledger_row, capabilities_provided);
    FIELD(weivel_ledger_row, services_consumed);
    FIELD(weivel_ledger_row, storage);
    FIELD(weivel_ledger_row, capabilities_consumed);
    FIELD(weivel_ledger_row, subject);
    FIELD(weivel_ledger_row, has_fix);
    SIZE(weivel_ledger_row);
    FIELD(weivel_declaration_refusal, size);
    FIELD(weivel_declaration_refusal, kind);
    FIELD(weivel_declaration_refusal, plugin);
    FIELD(weivel_declaration_refusal, id);
    FIELD(weivel_declaration_refusal, code);
    FIELD(weivel_declaration_refusal, subject);
    FIELD(weivel_declaration_refusal, has_fix);
    SIZE(weivel_declaration_refusal);
    FIELD(weivel_ledger_snapshot, size);
    FIELD(weivel_ledger_snapshot, rows);
    FIELD(weivel_ledger_snapshot, row_count);
    FIELD(weivel_ledger_snapshot, refusals);
    FIELD(weivel_ledger_snapshot, refusal_count);
    FIELD(weivel_ledger_snapshot, storage);
    SIZE(weivel_ledger_snapshot);
}

int main(void) {
    print_event_layouts();
    print_manifest_layouts();
    print_option_layouts();
    print_activation_layouts();
    print_call_layouts();
    print_answer_layouts();
    return fflush(stdout) == 0 ? 0 : 1;
}

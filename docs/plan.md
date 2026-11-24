# The activation plan

`weivel_plan_activation` answers the order a set of registered plugins starts in, the edges that order follows, the
cycles it cannot order and the plugins it will not start. It decides and starts nothing: starting the plugins of a plan
is the host's ([the host's activation](contract/host.md)). The declarations are in `include/weivel/core.h` and the code
is `src/activation.c`.

## Edges

A plugin's offers and needs are read from the manifest it was registered with, of the one kind the caller's edge source
names: capabilities (`WEIVEL_EDGES_CAPABILITIES`) or services (`WEIVEL_EDGES_SERVICES`). There is no default, because a
capability implies order on one tier and does not on another, and declarations of the other kind are ignored for
ordering and refusal alike. A consumed capability is always optional; a consumed service is optional or mandatory as its
manifest says.

A need another plugin of the set offers draws an edge from that plugin, the provider, to the one that needs it. A need
only the host offers draws none: the service ids the caller says the host supplies meet a need under service edges as a
plugin's offer would, never appear in the plan, and are ignored under capability edges. The plan's edges run between
plugins that will be considered for start, consumers in input order and each one's providers in the order its needs and
the input name them.

## Refusals

A plan refuses, in the order it finds them: a handle that names no plugin (`stale_handle`), a plugin listed again
(`repeated_plugin`), then each plugin with a mandatory need that nothing in the set or the host meets (`unmet_need`).
Whether a need is degradable never excuses an absence. An optional need nothing meets refuses nothing: the plugin
starts, and a want for that service waits. A refused plugin draws no edge and stops being a provider, so refusal runs
again until a pass refuses nothing more.

## Cycles

A cycle is at least two plugins that each wait, directly or not, on all the others. It is data, not a refusal: the
plan lists each cycle with its members, and a member is neither refused nor in the order. A cycle member stops being a
provider as a refused plugin does, so a plugin that needs one starts without it when the need is optional and is
refused `unmet_need` when it is mandatory. Every other plugin is still ordered.

## The order

The order is built in sweeps over the plugins as the caller listed them: each sweep places, in input order, every
plugin whose providers are all placed or in a cycle, and sweeps repeat until one places nothing. The host's own
services count as placed from the end of the first sweep, as though the host were listed last. So the same input
always answers the same order, not merely some order that puts each provider first.

## The answer

A plan, including one that refuses or cycles, is answered `WEIVEL_STATUS_OK` and queues nothing. Only a malformed call
is refused, answered `WEIVEL_STATUS_MALFORMED` with a refusal queued: a NULL core or plan, a plan below its first shape
or still holding one, NULL plugins with a count, an edge source that is neither kind, or a list of host services that
cannot be read. Every array and text of a plan lives in storage private to the core until
`weivel_activation_plan_release`, and each refusal's subject sits at the same index as the refusal.

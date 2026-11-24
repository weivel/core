# `handles`

The table from the core's handles to the language's objects: plugins, service registrations, implementations of
capabilities, watches and wants, and what each plugin holds of another's.

- An entry is made once the core answers the handle and before any event naming it is dispatched, and ends when what
  the handle stands for ends: an unregistration, a withdrawn implementation, a stopped watch, a settled or withdrawn
  want, a released or forgotten plugin. Making it cannot fail once the core has answered: the layer takes the room it
  needs before the call, so the core never holds an object whose handle the table lacks.
- A holding is the object a plugin was handed for another's registration or implementation, kept by the pair of the
  plugin's handle and that handle, a watch hearing a registration included. A `WEIVEL_EVENT_REVOKED` names the pair,
  and the layer drops its own entry for it. For a registration made with `weivel_register_service_awaiting_releases`
  the core keeps its holding past the revocation, and the layer ends it with `weivel_release_holding` once the
  language's owner of the object has let go of it, whichever way the plugin came to hold it. Its `plugin` may name a
  forgotten plugin, and is then only half of a key and never used as a handle to call with.
- A handle the table does not hold answers nothing. It is never matched to a newer object, because the core never
  reissues a handle while its object lives and refuses a stale one as data.
- A plugin may be opened with the layer's own token as its payload (`weivel_open_plugin`), so the layer reaches its
  record from a plugin handle without a search.
- It may not hold an object the core has stopped naming, nor answer one plugin's object to another unless the core
  answered that handle to it.

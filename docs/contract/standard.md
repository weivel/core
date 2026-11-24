# `standard` (optional)

The language's access to the standard's defaults through the same foreign function interface the core uses: reading
a JSON manifest into a manifest value, reading a library declaration, laying out and discovering plugins on disk,
identities, and the ids and definitions of the config, secrets and paths services (`services.h`), each translated
from the one C definition there.

- Every function here feeds the host: discovery answers manifest values and entries the caller hands to the host's
  registration.
- A repository may omit this module, and a host never requires it.
- It may not hold a default anywhere but in the standard's binary, nor add a format, a file name or a variable the
  standard does not own.

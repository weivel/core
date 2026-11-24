# `benchmarks`

The language's half of the platform's benchmarks, gating CI.

- Every bound is the kit's: a language reads its bounds from `benchmarks/<language>.txt` of the kit commit it pins,
  and no repository sets its own.
- The same-language gate, in every language, is the identity check: the service a consumer resolves is the provider's
  own object. The timing reported beside it is judged only against the noise bounds.
- The ratio gates hold the control call through the wrapper, and a C library's function called through the object the
  host registers for it, each over the same call made without the layer: through the bare binding in a language that
  crosses a foreign function interface, and in C through the bare table of the core's functions and the platform's
  loader.
- A C layer, which crosses no foreign function interface, also gates its control call through the wrapper against
  the core's own budget, the kit's `benchmarks/budget.txt`.
- A gate run on a platform the bounds give no line fails rather than passing unbounded.
- It may not loosen a bound, skip a gate, or pass with nothing measured.

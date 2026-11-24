# `conformance`

The driver for the layer suite.

- Every layer vector of the kit commit the language pins is witnessed by a test that names its id. The kit's `layers`
  drift check, run in the language's CI, fails when a vector's id appears nowhere in the language's test sources; the
  language's own coverage test, which reads its tests' names, stays the strong check.
- The driver builds each vector's plugins as manifest values and entries and registers them through the host's public
  surface, writing no file.
- The core's and the standard's vectors are the kit runner's; the layer suite runs only the layer's.
- It may not carry a vector of its own making in place of the kit's, nor a copy of a kit vector as a fixture.

The struct layouts of the public header, printed as this compiler lays them out, so a language that declares the same structs by hand checks its own offsets and sizes against the C compiler's rather than against a guess. The printer is checked against the header itself, so a struct or field added to the header and forgotten in the printer fails, and the check has tests of its own that plant each mistake it must catch in a copy of the headers, never in the real ones.

<!-- generated: source-children -->
- [layouts.c](../../../../test/layouts/layouts.c) - prints every public struct's fields with offset and size, and its sizeof, in a line format a language parses
- [layouts_check.cmake](../../../../test/layouts/layouts_check.cmake) - proves the printed lines cover every struct and field the headers declare, by name and in order
- [layouts_check_test.cmake](../../../../test/layouts/layouts_check_test.cmake) - plants one struct per scenario in a copy of the headers and runs the check against it
<!-- end generated -->

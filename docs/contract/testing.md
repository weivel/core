# `testing` (optional)

Helpers for the tests of a program that hosts plugins in the language, which name nothing of any one project.

- It lays out a scratch home and answers a free port, so a host program's tests do not each copy that fixture.
- It sets variables for a test and puts them back, and binds a described C library from its folder, so a test calls
  any described library, a project's own test library among them, with no binding written by hand.
- A repository may omit this module.
- It may not be needed by any other module, and it serves no one project: what a project needs of a home is that
  project's own layout, handed to it.

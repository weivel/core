# `testing` (optional)

Helpers for the tests of a program that hosts plugins in the language, which name nothing of any one project.

- It lays out a scratch home and answers a free port, so a host program's tests do not each copy that fixture.
- A repository may omit this module.
- It may not be needed by any other module, and it serves no one project: what a project needs of a home is that
  project's own layout, handed to it.

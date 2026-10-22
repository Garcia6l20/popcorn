# Contributing

## Before a change

Build, then run the gate stages that your change can affect:

```sh
uv run pcons -B build -j10
uv run python -m pytest -q -n 10 tests/python
```

`uv run pcons run check` runs every stage, see [Testing](testing.md). A change that reaches
every part, such as a core header, the build system, a shipped description or `pyproject.toml`,
needs the full run. A change to the docs needs the docs stage and `tests/docs`.

## Style

- C++ is C++26, built with GCC 16 and checked by `clang-format` with the repository's
  `.clang-format`. Use `std::move_only_function`, `std::copyable_function` or
  `std::function_ref` for callables, not `std::function`.
- Python is fully typed and checked by pyright. ruff and black use a line length of 100, and
  imports are sorted by ruff.
- Run the formatters in their fixing form first: `ruff check --fix`, `black` and
  `clang-format -i`.
- Comments: the only comments are doxygen `/** */` blocks on C++ declarations and docstrings in
  Python. The code carries the explanation. Public Python names use Google style docstrings
  with `Args`, `Returns` and `Raises`.
- A source file in `native` belongs to one library, and an include must follow the allowed
  edges in `popcorn_build/layering.py`, see [Architecture](architecture.md).

## Native code rules

- One native host thread runs a machine, see [Threads](threads.md). Nothing reports or logs on
  that thread, and only user callbacks take the GIL.
- Hold no host mutex while running user code, a destructor included.
- A Python object held natively goes through the callback table so that the garbage collector
  sees the cycle. Native code holds a weak relay, not the object.
- Validate arguments at the call, before anything is posted to the machine thread.
- Every per-call table, queue or record needs a stated bound.

## Tests

Every behavior change needs a test that fails without it. Break the code the test guards, see it
fail and restore it. Do not bound a test by wall time. See [Testing](testing.md).

## Commits

- The subject has under 80 characters, a leading capital and no trailing period. No
  Conventional Commits prefix.
- The body explains why. List the changes as bullets in the past tense.
- One commit per step. A review fix is a new commit on top, not a rewrite of an earlier one.

## Documentation

Guide snippets come from tests in `tests/docs` through `--8<--` sections, so a snippet is always
run. A public Python name needs a reference page, and `test_reference_coverage.py` fails when it
has none. Every declaration in the SDK headers that doxygen extracts needs a `/** */` comment.
Build the docs with `uv run python -m popcorn_build.docs`, see [Build](build.md).

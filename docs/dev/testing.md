# Testing

## Layout

| Directory | What it holds |
|---|---|
| `tests/native` | doctest programs for the C++ libraries, one folder per area: arch, core, image, obs, periph, stm32, plugin, consumers |
| `tests/firmware` | small firmware programs the native and Python tests run, with committed prebuilt ELFs |
| `tests/python` | pytest suites: api, description, image, native_plugin, obs, plugin, toolchain |
| `tests/data` | data files the tests read, such as a toy part description |
| `tests/docs` | the tests that supply the documentation snippets and guard the docs |
| `examples` | the example firmware projects, each with its own pytest suite |

## Native tests

`add_doctest` in `popcorn_build/testing.py` builds one program per suite and registers one test
per doctest case, so `pcons test` runs, filters and times out cases one by one:

```sh
uv run pcons -B build -j10
uv run pcons test -B build -j 10 --no-build -R 'core-(host|system)'
```

`-R` takes a regular expression over test names, which start with the group and the program,
such as `native.plugin-loader`. `--list` shows the names.

- A case that runs longer than its limit is killed and fails, so a deadlock fails the gate
  instead of hanging it. The default limit is 300 seconds, and a suite may set another. The
  asan and tsan variants multiply every limit by three.
- Programs link doctest with its own main. A program links one flavour of the popcorn
  libraries, shared or static, never both.
- `add_header_check` compiles every public header alone in its own translation unit, and all of
  them in one, so a header that needs another include fails the build. The header
  `popcorn/plugin/bindings.hpp` is excluded because it needs nanobind.
- The `tests/native/support` folder holds the shared fixtures for machines and firmware, and a
  smoke Python module.

## Firmware tests

The programs in `tests/firmware` are built with arm-none-eabi-gcc when it is on PATH, and the
committed ELF in `prebuilt` is used otherwise. When both exist the build compares their loadable
content and fails on a difference, see [Build](build.md). To change a firmware, edit its source,
build it and copy the new ELF over the prebuilt one.

## Python tests

```sh
uv run python -m pytest -q -n 10 tests/python
```

The suites use the popcorn pytest plugin, which boots each description once per process and
restores a snapshot per test. The suites in `tests/python/api` cover the public API. Tests that
run the machine must not depend on host load: a result may depend on the emulated input only,
never on wall time or on whether the machine was parked or running.

`tests/python/description` is split so that each directory runs under 120 s at `-n 6`:

| Directory | What it holds |
|---|---|
| `fresh` | each shipped part against a fresh build from pin data, and `ship --check` |
| `golden` | each committed golden against a build from ST's files |
| `shipped` | the shipped layers, their lookup, the cache, `bonded` and the aliases |
| `description_build` | description builds, die identity, facts and the H5 family |
| `patches` | patches, the binder, the SVD reader, the svd-parser oracle, IRQ names and the schema |

Set `POPCORN_STM32_PIN_DATA` to a STM32_open_pin_data checkout, or the `fresh` cases skip. The
`golden` cases need CubeMX and ST's SVDs.

Every test runs under a 15 s pytest-timeout limit, set in `pyproject.toml`. A test that needs
longer carries `@pytest.mark.timeout(seconds)`, above every wall bound it waits on. The limit uses
the thread method, so a test that exceeds it dumps every thread's stack and ends its worker
process: xdist reports the worker as crashed and starts a new one.

## Example tests

Each example has its own `pytest.ini` and runs from the repository root:

```sh
uv run python -m pytest -q examples/stm32u575/gpio
```

## Documentation tests

`tests/docs` holds the tests the guide pages take their snippets from, and tests that guard the
docs themselves:

- `test_reference_coverage.py` checks every public name has a reference page.
- `test_snippet_ranges.py` checks every line-range include covers whole blocks.
- `test_sdk_docs.py` checks the doxygen inputs and the stub and theme fallbacks.
- `test_readme.py` checks the README example is the quickstart test.

Run them with `uv run python -m pytest -q -n 10 tests/docs`.

## Checking a test

A test that cannot fail guards nothing. After writing one, break the behavior it guards, see it
fail, and restore the code. Keep the check cheap: rebuild only the target that holds the mutated
file and run only the test that should catch it. Python tests import the package from
`build/popcorn`, so a change in `python/popcorn` only takes effect after
`uv run pcons -B build`.

## The check command

`uv run pcons run check` is the whole gate, in this order: build, native tests, pytest on
`tests/python` and `tests/docs`, each example followed by its runs on the extra parts of
`popcorn_build/example_parts.py`, the examples together, the docs stage, pyright,
ruff, black, clang-format and the layering check. It stops at the first failing stage. `-j N` caps
the parallelism, and `--asan` runs the native suite in the asan variant only.

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
- `tests/data/core/m33_variants.py` derives the descriptors `m33_code_alias`, `m33_shared_page`
  and `model` from `tests/data/core/m33.json`. A build rule writes them into
  `build/test-data/m33` for the native tests. For the Python tests,
  `tests/python/derived_data.py` writes them at first use, once per content hash, into
  `popcorn-test-data-<uid>` under the temp directory. Edit the generator to change a variant.

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

Runs of the suites under `tests/` with `-n` use the xdist `worksteal` scheduler, set in
`tests/conftest.py`: a worker that runs out of tests takes half the queue of a busy one, so the long
description tests do not pile up on one worker. An explicit `--dist` or `-d`, from the command line,
`PYTEST_ADDOPTS` or the ini file, chooses another scheduler.

The description builder is memoised for the test session by `tests/build_memo.py`, installed from
`tests/conftest.py`: `build_description_with_report` runs once per distinct input in a `-n` run,
whichever worker asks first. The key is the part name and, for each file a `Sources` names and for
the package patch files, its path and bytes digest, so a test that edits a source or a patch in
`tmp_path` gets its own build. Each caller receives its own copy of the result, and the warnings the
build logged to the `popcorn` logger reach the caller's log capture again on a hit. The memo builds
through the builder itself while a module of the description package, `Path.read_bytes` or
`Path.read_text` is monkeypatched, and while the `popcorn` logger drops warnings. A test that spies
on any other call of the build, such as `open` or a parser, and asserts the call happens, carries
`@pytest.mark.no_build_memo` or runs the build inside `with build_memo.install().bypassed():`. The
marker covers the test and its function-scoped fixtures, a module or session fixture that builds
uses `bypassed()`.

The same memo holds `_svd.expand_svd`, keyed by the digest of the SVD bytes and the `file` argument.
The 226 shipped parts read 9 SVD files, so an SVD is expanded once per run and not once per part,
and each build still applies its own patches, rzone facts and bonding. A call that raises keeps
nothing. The two memos share the bypasses: a monkeypatched module of the description package, a
patched `Path.read_bytes` or `Path.read_text`, the marker and `bypassed()` give the real
`expand_svd` too, and a test that patches `_builder.expand_svd` itself gets its own function. The
bypass sees the attributes of the description package and `Path` reads. A test that patches
anything else on the build path, such as `copy.deepcopy` or `ET.fromstring`, carries the marker or
runs inside `bypassed()`.

`tests/python/description` is split so that each directory runs under 120 s at `-n 6`:

| Directory | What it holds |
|---|---|
| `fresh_u5` | each shipped U5 part against a fresh build from pin data, and the case that the U5, H5, L1 and L4 lists together hold every shipped part |
| `fresh_h5` | each shipped H5 part against a fresh build from pin data, `ship --check` and `build_each` |
| `fresh_l1` | each shipped L1 part against a fresh build from pin data |
| `fresh_l4` | each shipped L4 part against a fresh build from pin data |
| `golden` | each committed golden against a build from ST's files |
| `shipped` | the shipped layers, their lookup, the cache, `bonded` and the aliases |
| `description_build` | description builds, die identity, facts and the H5 family |
| `patches` | patches, the binder, the SVD reader, the svd-parser oracle, IRQ names and the schema |

Set `POPCORN_STM32_PIN_DATA` to a STM32_open_pin_data checkout, or the freshness and
`ship --check` cases of `fresh_u5`, `fresh_h5`, `fresh_l1` and `fresh_l4` skip. The `golden` cases
need CubeMX and ST's SVDs.

The CMSIS device headers come from the checkout. `tests/python/description/conftest.py` puts the
`build/deps/src/cmsis-device-*/Include` directories the configure fetched first in
`POPCORN_CMSIS_DEVICE_DIRS` and keeps the entries already set. The configure fetches them only when
it finds `arm-none-eabi-gcc`. With `arm-none-eabi-gcc` on PATH and a pack missing, the run stops
with a usage error that asks for the configure. Without the cross compiler the L1 and L4 header,
golden and freshness cases skip, and the H5 cases skip unless the variable names another
`stm32h573xx.h`.

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

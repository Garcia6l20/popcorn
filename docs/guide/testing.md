# Testing with pytest

The `popcorn.pytest` plugin boots each machine description once per process and restores the
boot snapshot before every test that uses it. A test starts from the same state each time, at
the cost of a restore, not a boot.

pytest loads the plugin through its entry point wherever popcorn is installed with the `pytest`
extra. No conftest line is needed.

## Declare a machine fixture

```python
--8<-- "guide/test_testing.py:fixture"
```

`@popcorn.pytest.machine` turns a function that returns a `MachineDescription` into a fixture
named after the function. A test that takes a `button` parameter gets a booted `popcorn.Machine`.
Declare fixtures in a `conftest.py` to share them across test files.

## Write tests

Name the file `test_*.py` and run it as usual:

```sh
uv run pytest tests/
```

`ELF` in the snippets is the path of your firmware, see [First machine](first_machine.md). A helper drives the
button through the GPIO pin view. The button has a pull-up, so the pin rests high. Driving it
to 0 is the falling edge that fires the interrupt, and driving it back to 1 releases it:

```python
--8<-- "guide/test_testing.py:press"
```

Each test starts from the boot snapshot, whatever the test before it did:

```python
--8<-- "guide/test_testing.py:restored"
```

A coroutine test runs on the plugin's event loop. pytest-asyncio is not needed. Before each test
the plugin also removes the hooks the previous test installed and empties its intercept logs and
trace ring.

## Bound every run with a marker

```python
--8<-- "guide/test_markers.py:marker"
```

`@pytest.mark.popcorn` sets the settings of every popcorn machine a test uses:

- `budget`, the default instruction budget of each `run`, `call` and wait. `budget=None` means
  no limit. It never acts as a wall-clock timeout.
- `strict`, whether a firmware access to an unmodelled peripheral raises `ModelError`.

Put it on a module as `pytestmark = pytest.mark.popcorn(budget=...)`. The closest marker wins
whole: a function's marker replaces its module's. On the command line, `--popcorn-budget` and
`--popcorn-strict` apply where no marker sets the value.

## More machines in one test

```python
--8<-- "guide/test_testing.py:factory"
```

The `machine_factory` fixture builds new machines that belong to one test. `await
machine_factory.booted(description)` returns a machine restored from the boot snapshot of that
description, shared with its fixtures. Use it from async tests. The sync call
`machine_factory(description)` does the same once the description has booted.
`machine_factory(description, booted=False)` returns a machine powered off and not reset, for a
test that boots it itself:

```python
--8<-- "guide/test_testing.py:sync"
```

A `MachineDescription` with `missing="skip"` skips the tests that use it when its ELF or an ST
file it needs is missing, instead of failing them. Use it for firmware a contributor may not have
built.

`@popcorn.pytest.system` declares a fixture for a linked system the same way, see
[Systems and links](systems.md).

## When a test fails

The report of a failing test has a "popcorn" section per machine: the last stop, the decoded
fault registers and the call stack.

- `--popcorn-ring=N` adds the last `N` records of the machine's trace ring: MMIO accesses,
  interrupts, calls and stops.
- `--popcorn-trace-on-fail=DIR` traces every machine fixture and keeps the trace of each failed
  test in `DIR`.
- `--popcorn-cov` collects firmware coverage of every machine the plugin builds. The lcov file
  written at the end of the session is `--popcorn-cov-report=lcov:PATH` or the `popcorn_cov_lcov`
  ini option. `--popcorn-cov-include=GLOB` and `--popcorn-cov-omit=GLOB` filter the source files,
  and a command-line value replaces the ini value of the same setting.

## Boot cache

Boot snapshots are cached in `.pytest_cache/d/popcorn/boot`. A later session, or another xdist
worker, restores a boot instead of running it. The cache key covers the popcorn build, the
description, the images and the model sources. `--cache-clear` drops the cache.

At session end, the plugin deletes the entries that can no longer hit: those booted from another
popcorn version or native extension, or from an image that changed or no longer exists. It also
deletes entries unused for `popcorn_cache_max_age_days` days, a cache hit counting as a use:

```ini
[pytest]
popcorn_cache_max_age_days = 14
```

The default is 7 days and 0 disables the age limit. Under xdist only the controller sweeps, and
an entry another process is booting or restoring is left for the next session. Files of a key
without a json, such as partial writes, are deleted once older than the age limit.

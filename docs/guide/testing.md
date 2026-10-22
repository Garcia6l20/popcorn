# Testing with pytest

The `popcorn.pytest` plugin boots each machine description once per session and restores the
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

A helper drives the button through the GPIO pin view:

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
--8<-- "guide/test_testing.py:marker"
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
test that boots it itself.

## When a test fails

The report of a failing test has a "popcorn" section per machine: the last stop, the decoded
fault registers and the call stack.

- `--popcorn-ring=N` adds the last `N` records of the machine's trace ring: MMIO accesses,
  interrupts, calls and stops.
- `--popcorn-trace-on-fail=DIR` traces every machine fixture and keeps the trace of each failed
  test in `DIR`.
- `--popcorn-cov=LCOV` collects firmware coverage of every machine the plugin builds and writes
  it as an lcov file at the end of the session.

## Boot cache

Boot snapshots are cached in `.pytest_cache/d/popcorn/boot`. A later session, or another xdist
worker, restores a boot instead of running it. The cache key covers the popcorn build, the
description, the images and the model sources. `--cache-clear` drops the cache.

"""pytest plugin for popcorn machines.

Load it from the root `conftest.py` of a test suite:

    pytest_plugins = ["popcorn.pytest"]

Fixtures:

    async def test_boot_state(machine: popcorn.Machine) -> None:
        await machine.call("check")

- `popcorn_project`, session scope: the `popcorn.Project` of `--popcorn-project`, by default
  `popcorn.toml` in the pytest rootdir.
- `machine`: the project's machine, built and booted once per session and process, then
  restored from its boot snapshot before each test. Every test gets the same object. The plugin
  removes the hooks a test installed, empties the intercept logs, the call baseline and the
  trace ring. A test that loads another image, opens a pty or leaves the machine running spoils
  it: the next test gets a new machine restored from the saved boot. A project without
  `[boot] until` boots to the reset state. A failed boot errors every test using the machine,
  booted once.
- `machine_factory`: a `MachineFactory`, new machines separate from `machine`.

A failing test that used a popcorn machine gets a "popcorn" section in its report, one block per
machine: the stop of the failing exception or of the machine's last run, the fault registers
decoded, the call stack and the trace ring. A machine without a stop since the test started shows
its registers and ring. The report of a setup error from a failed boot shows the boot's stop.

With `--popcorn-trace-on-fail=DIR`, the `machine` fixture traces every test with
`popcorn.trace.ALL` and keeps `DIR/<node id>.pctr` for a test whose call failed, named in its
report. The bare flag keeps them in `.pytest_cache/d/popcorn/traces`, or `.popcorn-traces` in the
rootdir without the cache plugin. Write it as `--popcorn-trace-on-fail=DIR` or last on the command
line: a bare flag before a path takes the path as its DIR. `machine_factory` machines are not
traced. Kept traces are never pruned: `--cache-clear` removes those in the pytest cache.

With `--popcorn-cov=LCOV`, every machine the plugin builds counts coverage. Each process adds
its machines' counts to one `.pcov` file of a temporary `popcorn-cov-*` directory, the boot of
each configuration counted once, and the controller merges them into `LCOV` at session end. An
interrupted session keeps the directory and writes no lcov. A crash or SIGKILL leaves the
directory behind. xdist workers on other hosts, `--tx ssh=...`, are not supported: they share
the directory by path.

Boot snapshots are cached in `.pytest_cache/d/popcorn/boot`: a later session, or another xdist
worker, restores a configuration's boot instead of running it, one boot per configuration across
workers. The cache key covers the popcorn build, the description, the images, the project file
with its config, the strictness, the `[models] python` directories and the source of every board
model and device class. A module outside those directories that a model imports is not covered,
and entries are never pruned: `--cache-clear` drops them all. `-p no:cacheprovider` turns the
cache off.

Limits:

- Host input queued on a UART and not delivered yet stays queued across the restore.
- A trace opened with `machine.trace(...)` and never closed keeps its sink on the reused machine.
- The `[models] python` directories stay on `sys.path` for the session.

Async tests:

    async def test_ticks() -> None:
        await asyncio.sleep(0)

A coroutine test function runs on the plugin's own event loop, one `asyncio.Runner` per session
and process, when no other plugin claimed it first. pytest-asyncio is not needed. Tasks a test
leaves running are cancelled after it, with a warning.

Async fixtures, coroutines or async generators of any scope, run on the same loop when no other
plugin claimed them: a session-scoped async fixture and every test share one loop per process.
They are requested as parameters: `request.getfixturevalue` of an async fixture from inside an
async test cannot run it, the loop is already running. Each test and fixture runs in its own copy
of the context. The context variables a fixture sets reach the tests using it and are reset at
its teardown, those a test sets are gone after it. A fixture owns the tasks it starts and
cancels them itself: one still running when the fixture ends lives until the session closes.

With pytest-asyncio installed, it keeps the tests and fixtures it claims: in its strict mode
those marked `@pytest.mark.asyncio` and the `@pytest_asyncio.fixture` ones, in auto mode every
async test and fixture. They run on its loops, everything else on popcorn's. Mixing traps:

- A test pytest-asyncio runs uses pytest-asyncio fixtures for its async state. A plain async
  fixture it requests in strict mode runs on popcorn's loop, a different one.
- An unmarked test popcorn runs, in strict mode, that requests a `@pytest_asyncio.fixture` gets a
  value created on pytest-asyncio's loop. Loop-bound objects in it break.
- A `popcorn.Machine` is bound to no loop: one built on either loop runs on the other.

Tests and fixtures anyio's plugin runs, those using its `anyio_backend` fixture, are left to it.

Options, registered for `pytest --help`: `--popcorn-project`, `--popcorn-config`,
`--popcorn-cov`, `--popcorn-trace-on-fail[=DIR]`, `--popcorn-strict`, `--popcorn-budget`.

    @pytest.mark.popcorn(config="variant", budget=1_000_000, strict=True)
    async def test_variant(machine: popcorn.Machine) -> None: ...

Each setting of a test comes from its `popcorn` marker, else the command line, else the
project file:

- `config`: `--popcorn-config`, else the base file. `config=None` in a marker is the base file
  even with `--popcorn-config`. It also selects the config of `popcorn_project`.
- `strict`: None or absent, the command line then the project file; True or False overrides.
  `--popcorn-strict` turns strictness on, else `[models] strict` applies.
- `budget`: `--popcorn-budget`, the `default_budget` of the test's booted machines, per `run`,
  `wait_for` and `call`, never a wall-clock timeout. `budget=None` in a marker is unbounded.

The closest marker wins whole, the function's over its class's over its module's: its keywords
do not merge with those of a farther marker. Every marked test has its marker checked at setup,
whether it uses a popcorn fixture or not.
"""

from popcorn.pytest._fixtures import MachineFactory
from popcorn.pytest._options import Settings

pytest_plugins = [
    "popcorn.pytest._options",
    "popcorn.pytest._loop",
    "popcorn.pytest._session",
    "popcorn.pytest._fixtures",
    "popcorn.pytest._report",
    "popcorn.pytest._coverage",
]

__all__ = ["MachineFactory", "Settings"]

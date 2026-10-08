"""pytest plugin for popcorn machines.

pytest loads it by its `pytest11` entry point wherever popcorn is installed, no conftest needed.
`-p no:popcorn.pytest` turns it off. With `PYTEST_DISABLE_PLUGIN_AUTOLOAD=1`, load it from the
root `conftest.py` of a test suite:

    pytest_plugins = ["popcorn.pytest"]

Fixtures:

    # conftest.py
    @pytest.fixture(scope="session")
    def popcorn_project() -> popcorn.Project:
        return popcorn.Project(part="STM32U575QGIx", images=[popcorn.Elf(HERE / "app.elf")])

    # test_app.py
    async def test_boot_state(machine: popcorn.Machine) -> None:
        await machine.call("check")

- `popcorn_project`: the test's `popcorn.Project`, defined by the user in a conftest.py. The
  nearest override above a test wins, as for any pytest fixture. The plugin's own fails the
  tests that use a machine, naming the fix.
- `popcorn_selected_project`: `popcorn_project` with the test's config applied, selected as for
  `machine`: the marker's, else `--popcorn-config`, else the base project. For tests that boot
  machines themselves.
- `machine`: the test's project's machine, built and booted once per session and process, then
  restored from its boot snapshot before each test. Every test gets the same object. The plugin
  removes the hooks a test installed, empties the intercept logs, the call baseline and the
  trace ring. A test that loads another image, opens a pty or leaves the machine running spoils
  it: the next test gets a new machine restored from the saved boot. A project without
  `boot.until` boots to the reset state. A failed boot errors every test using the machine,
  booted once.
- `machine_factory`: a `MachineFactory`, new machines separate from `machine`, with the
  project's bus devices or the test's own, from the test's project or another one.

A boot is keyed by the `Project.digest` of the project with its config applied, the
strictness and the bus devices given. One session can run the tests of several conftest
directories, each project booted once per key and process. Two config names with equal
content share one boot. The report section names a key by the project's `name`, the config
name and the first 12 hex digits of the digest.

A failing test that used a popcorn machine gets a "popcorn" section in its report, one block per
machine: the stop of the failing exception or of the machine's last run, the fault registers
decoded and the call stack. A machine without a stop since the test started shows its registers.
The report of a setup error from a failed boot shows the boot's stop. With `--popcorn-ring=N`,
each block ends with the last N lines of the machine's trace ring, a group of up to 8 records
repeated back to back printed once with its repeat count: a poll loop takes a few lines. Without
it, a one-line hint names the option.

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

xdist workers are spawned processes, not forks of the controller, so each worker builds its own
machines after it starts, as a machine requires: it does not survive `os.fork`.

The lcov file uses the lcov 2 function records, `FN:<start>,<end>,<name>`. `genhtml
--branch-coverage` reads it with no ignored error but one, which the data can legitimately
produce: `line is hit but no branches on line have been evaluated`. It names a line whose branch
never ran while another of its instructions did: a shared exit the line table gives the branch's
line, or a run that stopped or faulted after the line's first instruction and before its branch.
`--ignore-errors inconsistent` renders it, and turns genhtml's other consistency errors into
warnings too.

Boot snapshots are cached in `.pytest_cache/d/popcorn/boot`: a later session, or another xdist
worker, restores a configuration's boot instead of running it, one boot per configuration across
workers. The cache key covers the popcorn build, the description, the images, the project
digest, the strictness, the source of every board model and device class and of the modules its
source file imports outside popcorn, the Python installation and site-packages, the files
their parameters name, and for each peripheral without a board model the
`popcorn.model.register()` registration that serves it, with its class source and imports in
the same way. A module imported only by those modules is not covered, and entries are
never pruned: `--cache-clear` drops them all. `-p no:cacheprovider` turns the cache off.

Limits:

- Host input queued on a UART and not delivered yet stays queued across the restore.
- A trace opened with `machine.trace(...)` and never closed keeps its sink on the reused machine.

Async tests:

    from popcorn.units import ms

    async def test_ticks(machine: popcorn.Machine) -> None:
        await machine.run(for_=ms(1))

A coroutine test function runs on the plugin's own event loop, one `asyncio.Runner` per session
and process, when no other plugin claimed it first. pytest-asyncio is not needed. Tasks a test
leaves running are cancelled after it, with a warning. Machines run on their own threads, the
loop only awaits them. Teardown parks the `machine` and `machine_factory` machines a test left
started, not machines the test built itself.

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

Options, registered for `pytest --help`: `--popcorn-config`, `--popcorn-cov`,
`--popcorn-trace-on-fail[=DIR]`, `--popcorn-strict`, `--popcorn-budget`, `--popcorn-ring`.

    @pytest.mark.popcorn(config="variant", budget=1_000_000, strict=True)
    async def test_variant(machine: popcorn.Machine) -> None: ...

Each setting of a test comes from its `popcorn` marker, else the command line, else the
project:

- `config`: a name in `popcorn_project.configs`, from `Project.with_configs`, else
  `--popcorn-config`, else the base project. `config=None` in a marker is the base project even
  with `--popcorn-config`. An unknown name fails the test, naming the known ones.
  `popcorn_project` itself stays the user's value, no config applied.
  `popcorn_selected_project` is the one with the config applied.
- `strict`: None or absent, the command line then the project; True or False overrides.
  `--popcorn-strict` turns strictness on, else `Project.strict` of the config applies.
- `budget`: `--popcorn-budget`, the `default_budget` of the test's booted machines, per `run`,
  `wait_for` and `call`, never a wall-clock timeout. `budget=None` in a marker is unbounded.

The closest marker wins whole, the function's over its class's over its module's: its keywords
do not merge with those of a farther marker. Every marked test has its marker checked at setup,
whether it uses a popcorn fixture or not.
"""

import pytest

pytest_plugins = [
    "popcorn.pytest._options",
    "popcorn.pytest._loop",
    "popcorn.pytest._session",
    "popcorn.pytest._fixtures",
    "popcorn.pytest._report",
    "popcorn.pytest._coverage",
]

pytest.register_assert_rewrite(*pytest_plugins)

from popcorn.pytest._fixtures import MachineFactory  # noqa: E402
from popcorn.pytest._options import Settings  # noqa: E402

__all__ = ["MachineFactory", "Settings"]

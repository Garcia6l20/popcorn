"""pytest plugin for popcorn machines.

pytest loads it by its `pytest11` entry point wherever popcorn is installed, no conftest needed.
`-p no:popcorn.pytest` turns it off. With `PYTEST_DISABLE_PLUGIN_AUTOLOAD=1`, load it from the
root `conftest.py` of a test suite:

    pytest_plugins = ["popcorn.pytest"]

Machine and system fixtures are declared in a conftest.py, any number of them, each named
after its function:

    # conftest.py
    import popcorn
    import popcorn.pytest

    CONTROLLER = popcorn.MachineDescription(
        part="STM32U575QGIx", images=[popcorn.Elf(HERE / "app.elf")]
    )

    @popcorn.pytest.machine
    def controller() -> popcorn.MachineDescription:
        return CONTROLLER

    @popcorn.pytest.machine(config="variant")
    def controller_variant() -> popcorn.MachineDescription:
        return CONTROLLER

    @popcorn.pytest.system
    def linked() -> popcorn.SystemDescription:
        return popcorn.SystemDescription(
            members={"ctl": CONTROLLER, "r1": RESPONDER, "r2": RESPONDER},
            links=[
                popcorn.UartLink("ctl.UART4", "r1.USART3", latency=us(20), baud=460_800),
                popcorn.UartLink("ctl.USART3", "r2.USART3", latency=us(20), baud=460_800),
            ],
        )

    # test_app.py
    async def test_x(controller: popcorn.Machine, linked: popcorn.System) -> None:
        await controller.call("check")

The decorated function is a fixture function: it may request other fixtures, `request`
included, and be a coroutine function. It returns the description, it does not yield it. Both
decorators take `params=` and `ids=` as `pytest.fixture` does, `request.param` in the function.
The nearest declaration above a test wins, as for any pytest fixture. Keep descriptions shared
by several conftest.py files in a plain module with a unique name, not in a conftest.py.

- `@popcorn.pytest.machine`: a booted machine of the description, its config applied. The key,
  the description with its config, the strictness and the bus devices, boots once per session
  and process, shared by every fixture and `machine_factory` call of that key. Each fixture name
  has its own machine per key, built named after the fixture and reused by every test: before
  each test it is restored from the boot snapshot, its hooks removed, its intercept logs, call
  baseline and trace ring emptied. Two fixtures of one description in one test are two machines
  of one boot. A test that loads another image or opens a pty spoils the machine: the next test
  gets a new one restored from the saved boot. A description without `boot.until` boots to the
  reset state. A failed boot errors every test using the key, booted once. The config is set on
  the decorator only, `config=None` for the base description.
- `@popcorn.pytest.system`: the description booted and linked, a `popcorn.System`, new for each
  test. Each member is a new machine named as the member, restored from the boot snapshot of its
  own description's key, so a member and a machine fixture of the same description and
  strictness share one boot. A member's config is part of its description,
  `SystemDescription(members={name: description.config(...)})`. Teardown halts a free run left
  started, closes the system, then parks and closes the members.
- `machine_factory`: a `MachineFactory`, new machines for one test, never a fixture's, from a
  description given at each call: `machine_factory(CONTROLLER, config="variant")`,
  `await machine_factory.booted(CONTROLLER)` from an async test, `booted=False` for a machine
  powered off and not reset, `devices=` for bus devices of the test's own.

Teardown parks the machines a test left started: what ended a free run by itself and never
reached the test fails the teardown.

A machine fixture's machine is named after the fixture in its log records and errors, a
system member after its member key, a `machine_factory` machine after its `name=` or the part.
A failing test reports a machine fixture under its name, a system member as
`<fixture> member <name>`, a factory machine as `machine_factory #<n>`.

A description with `missing="skip"` skips the tests using it when an input is missing, the
missing path as the reason.

A boot is keyed by the `MachineDescription.digest` of the machine description with its config
applied, the strictness and the bus devices given. One session can run the tests of several conftest
directories, each machine description booted once per key and process. Two config names with equal
content share one boot. The report section names a key by the machine description's `name`, the
config name and the first 12 hex digits of the digest.

A failing test that used a popcorn machine gets a "popcorn" section in its report, one block per
machine: the stop of the failing exception or of the machine's last run, the fault registers
decoded and the call stack. A machine without a stop since the test started shows its registers.
The report of a setup error from a failed boot shows the boot's stop. With `--popcorn-ring=N`,
each block ends with the last N lines of the machine's trace ring, a group of up to 8 records
repeated back to back printed once with its repeat count: a poll loop takes a few lines. Without
it, a one-line hint names the option.

With `--popcorn-trace-on-fail=DIR`, every machine fixture and system member is traced for
every test with `popcorn.trace.ALL`. A test whose call failed keeps
`DIR/<node id>.<fixture>.pctr` per machine fixture and `DIR/<node id>.<fixture>.<member>.pctr`
per system member, named in its report. The bare flag keeps them in
`.pytest_cache/d/popcorn/traces`, or `.popcorn-traces` in the rootdir without the cache plugin.
Write it as `--popcorn-trace-on-fail=DIR` or last on the command line: a bare flag before a path
takes the path as its DIR. `machine_factory` machines are not traced. Kept traces are never
pruned: `--cache-clear` removes those in the pytest cache.

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
workers. The cache key covers the popcorn build, the description, the images, the machine
description digest, the strictness, the source of every board model and device class and of the
modules its source file imports outside popcorn, the Python installation and site-packages, the
files their parameters name, and for each peripheral without a board model the
`popcorn.model.register()` registration that serves it, with its class source and imports in
the same way. A module imported only by those modules is not covered, and entries are
never pruned: `--cache-clear` drops them all. `-p no:cacheprovider` turns the cache off.

Limits:

- Host input queued on a UART and not delivered yet stays queued across the restore.
- A trace opened with `machine.trace(...)` and never closed keeps its sink on the reused machine
  of a machine fixture.

Async tests:

    from popcorn.units import ms

    async def test_ticks(controller: popcorn.Machine) -> None:
        await controller.run(for_=ms(1))

A coroutine test function runs on the plugin's own event loop, one `asyncio.Runner` per session
and process, when no other plugin claimed it first. pytest-asyncio is not needed. Tasks a test
leaves running are cancelled after it, with a warning. Machines run on their own threads, the
loop only awaits them. Teardown parks the fixture and `machine_factory` machines a test left
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

Options, registered for `pytest --help`: `--popcorn-cov`, `--popcorn-trace-on-fail[=DIR]`,
`--popcorn-strict`, `--popcorn-budget`, `--popcorn-ring`.

    @pytest.mark.popcorn(budget=1_000_000, strict=True)
    async def test_bounded(controller: popcorn.Machine, linked: popcorn.System) -> None: ...

The `popcorn` marker applies to every popcorn machine of the test: its machine fixtures, system
members and `machine_factory` machines. Each setting comes from the marker, else the command
line, else the description:

- `strict`: None or absent takes the command line, then the description. True or False overrides.
  `--popcorn-strict` turns strictness on, else `MachineDescription.strict` of the config
  applies. A `strict=` given to `machine_factory` overrides all three.
- `budget`: `--popcorn-budget`, the `default_budget` of the test's booted machines, per `run`,
  `wait_for` and `call`, never a wall-clock timeout. `budget=None` in a marker is unbounded.
  `system.run` is bounded by its `for_` or `until` only.

The marker takes no `config`: a marker naming one fails the test, the config is set on
`@popcorn.pytest.machine(config=...)` or passed to `machine_factory`. The closest marker wins
whole, the function's over its class's over its module's: its keywords do not merge with those
of a farther marker. Every marked test has its marker checked at setup, whether it uses a
popcorn fixture or not.
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

from popcorn.pytest._declare import machine, system  # noqa: E402
from popcorn.pytest._fixtures import MachineFactory  # noqa: E402
from popcorn.pytest._options import Settings  # noqa: E402

__all__ = ["MachineFactory", "Settings", "machine", "system"]

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
Marker: `@pytest.mark.popcorn(config=None, budget=None, strict=None)`.
"""

from popcorn.pytest._fixtures import MachineFactory
from popcorn.pytest._options import Settings

pytest_plugins = [
    "popcorn.pytest._options",
    "popcorn.pytest._loop",
    "popcorn.pytest._session",
    "popcorn.pytest._fixtures",
]

__all__ = ["MachineFactory", "Settings"]

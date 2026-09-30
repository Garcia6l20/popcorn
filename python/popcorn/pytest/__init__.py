"""pytest plugin for popcorn machines.

Load it from the root `conftest.py` of a test suite:

    pytest_plugins = ["popcorn.pytest"]

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

from popcorn.pytest._options import Settings

pytest_plugins = ["popcorn.pytest._options", "popcorn.pytest._loop"]

__all__ = ["Settings"]

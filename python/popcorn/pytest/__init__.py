"""pytest plugin for popcorn machines.

Load it from the root `conftest.py` of a test suite:

    pytest_plugins = ["popcorn.pytest"]

Async tests:

    async def test_ticks() -> None:
        await asyncio.sleep(0)

A coroutine test function runs on the plugin's own event loop, one `asyncio.Runner` per session
and process, when no other plugin claimed it first. pytest-asyncio is not needed. Tasks a test
leaves running are cancelled after it, with a warning.

Options, registered for `pytest --help`: `--popcorn-project`, `--popcorn-config`,
`--popcorn-cov`, `--popcorn-trace-on-fail[=DIR]`, `--popcorn-strict`, `--popcorn-budget`.
Marker: `@pytest.mark.popcorn(config=None, budget=None, strict=None)`.
"""

from popcorn.pytest._options import Settings

pytest_plugins = ["popcorn.pytest._options", "popcorn.pytest._loop"]

__all__ = ["Settings"]

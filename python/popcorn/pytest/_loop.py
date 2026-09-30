"""The plugin's own event loop: async tests nobody else runs."""

from __future__ import annotations

import asyncio
import inspect
import warnings
from collections.abc import Callable, Coroutine, Mapping
from dataclasses import dataclass
from typing import cast

import pytest

type AsyncTest = Callable[..., Coroutine[object, object, object]]


class OwnedLoop:
    """One `asyncio.Runner` per session and process, created on first use.

    Async tests run on it one after the other. Between two tests the runner's loop is the
    thread's current event loop, not running, so a sync test may call `asyncio.run` in between.
    """

    def __init__(self) -> None:
        self._runner: asyncio.Runner | None = None

    def run_test(self, function: AsyncTest, arguments: Mapping[str, object]) -> None:
        """Run the async test `function` with `arguments` to completion.

        Tasks the test started and left running are cancelled after it, with one
        `PytestWarning` naming them and any error they raised, so they never run inside the
        next test. A task still running `CANCEL_WAIT` seconds after its cancellation is named in
        the warning and left on the loop: it never blocks the session.
        """
        self._get().run(_guarded(function(**arguments)))

    def close(self) -> None:
        """Close the runner, cancelling what is left on the loop within `CANCEL_WAIT` seconds."""
        runner, self._runner = self._runner, None
        if runner is None:
            return
        remaining = runner.run(_cancel_all())
        if not remaining.running:
            runner.close()
            return
        asyncio.set_event_loop(None)
        runner.get_loop().close()

    def _get(self) -> asyncio.Runner:
        if self._runner is None:
            self._runner = asyncio.Runner()
        return self._runner


CANCEL_WAIT = 5.0
"""Seconds a cancelled task has to finish, a hang guard."""


@dataclass(frozen=True, slots=True)
class _Cancelled:
    tasks: list[asyncio.Task[object]]
    running: list[asyncio.Task[object]]
    errors: list[BaseException]


def _current() -> asyncio.Task[object] | None:
    try:
        return asyncio.current_task()
    except RuntimeError:
        return None


async def _cancel(tasks: set[asyncio.Task[object]]) -> _Cancelled:
    current = _current()
    if current is not None:
        tasks.discard(current)
    if not tasks:
        return _Cancelled([], [], [])
    for task in tasks:
        task.cancel()
    done, pending = await asyncio.wait(tasks, timeout=CANCEL_WAIT)
    errors = [
        error for task in done if not task.cancelled() and (error := task.exception()) is not None
    ]
    return _Cancelled(_by_name(tasks), _by_name(pending), errors)


def _by_name(tasks: set[asyncio.Task[object]]) -> list[asyncio.Task[object]]:
    return sorted(tasks, key=lambda task: task.get_name())


async def _cancel_all() -> _Cancelled:
    return await _cancel(asyncio.all_tasks())


async def _guarded(test: Coroutine[object, object, object]) -> None:
    before = asyncio.all_tasks()
    try:
        await test
    finally:
        cancelled = await _cancel(asyncio.all_tasks() - before)
        if cancelled.tasks:
            warnings.warn(pytest.PytestWarning(_describe(cancelled)), stacklevel=1)


def _describe(cancelled: _Cancelled) -> str:
    names = ", ".join(task.get_name() for task in cancelled.tasks)
    text = f"popcorn cancelled {len(cancelled.tasks)} task(s) the test left running: {names}"
    if cancelled.running:
        running = ", ".join(task.get_name() for task in cancelled.running)
        text += f"; still running after {CANCEL_WAIT:g} s: {running}"
    if cancelled.errors:
        text += "; errors: " + ", ".join(repr(error) for error in cancelled.errors)
    return text


OWNED_LOOP = pytest.StashKey[OwnedLoop]()
_FIXTURE_INFO = "_fixtureinfo"


def owned_loop(config: pytest.Config) -> OwnedLoop:
    """The session's owned loop of `config`."""
    loop = config.stash.get(OWNED_LOOP, None)
    if loop is None:
        loop = config.stash[OWNED_LOOP] = OwnedLoop()
    return loop


def call_arguments(item: pytest.Function) -> dict[str, object]:
    """The keyword arguments pytest passes to the test function of `item`.

    pytest's own rule, its fixture info's argument names, which leave out the parameters
    `mock.patch` fills and those with a default value.
    """
    names = cast(tuple[object, ...], getattr(item, _FIXTURE_INFO).argnames)
    assert isinstance(names, tuple) and all(isinstance(name, str) for name in names)
    return {name: item.funcargs[name] for name in cast(tuple[str, ...], names)}


@pytest.hookimpl(tryfirst=True)
def pytest_pyfunc_call(pyfuncitem: pytest.Function) -> bool | None:
    """Run a coroutine test function on the owned loop, unless anyio's plugin runs it."""
    function: object = pyfuncitem.obj
    if not inspect.iscoroutinefunction(function) or "anyio_backend" in pyfuncitem.funcargs:
        return None
    owned_loop(pyfuncitem.config).run_test(function, call_arguments(pyfuncitem))
    return True


def pytest_unconfigure(config: pytest.Config) -> None:
    """Close the owned loop."""
    loop = config.stash.get(OWNED_LOOP, None)
    if loop is not None:
        loop.close()

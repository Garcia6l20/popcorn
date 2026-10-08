"""Bridge from a host request, completed on a machine's thread, to the awaiting event loop."""

from __future__ import annotations

import asyncio
import contextlib
import itertools
import weakref
from collections.abc import Callable

from popcorn import _core


class _Waker:
    """The loop waker of one event loop: an eventfd the loop reads, and the futures waiting on
    it by token. A machine's thread queues a token without the GIL, the loop resolves the
    future. Holds the loop weakly: a waker referencing its loop would keep it alive forever."""

    __slots__ = ("__weakref__", "native", "waiting")

    def __init__(self) -> None:
        self.native = _core.LoopWaker()
        self.waiting: weakref.WeakValueDictionary[int, asyncio.Future[None]] = (
            weakref.WeakValueDictionary()
        )

    def drain(self) -> None:
        """Resolve the futures whose tokens were queued, in completion order."""
        for token in self.native.drain():
            waiting = self.waiting.pop(token, None)
            if waiting is not None and not waiting.done():
                waiting.set_result(None)


_WAKERS: weakref.WeakKeyDictionary[asyncio.AbstractEventLoop, _Waker | None] = (
    weakref.WeakKeyDictionary()
)
_TOKENS = itertools.count(1)


def _waker_of(loop: asyncio.AbstractEventLoop) -> _Waker | None:
    """The waker of `loop`, made and watched on first use. None for a loop whose `add_reader`
    raises NotImplementedError: its wakes need the GIL on the machine's thread."""
    if loop in _WAKERS:
        return _WAKERS[loop]
    waker: _Waker | None = _Waker()
    weak = weakref.ref(waker)

    def drain() -> None:
        found = weak()
        if found is not None:
            found.drain()

    try:
        loop.add_reader(waker.native.fd(), drain)
    except NotImplementedError:
        waker = None
    _WAKERS[loop] = waker
    return waker


async def awaited[T](
    future: _core.HostFuture, convert: Callable[[object], T], *, cancel: bool = True
) -> T:
    """Await a host request on the running loop, which the request is bound to only here.

    A request already done returns without yielding. Cancelling the awaiting task cancels the
    request unless `cancel` is False: a goal halts at its next boundary, a wait is dropped, a
    command not yet run is withdrawn.

    The machine's thread wakes the loop through the loop's eventfd, without the GIL, so a loop
    that never blocks, spinning on `asyncio.sleep(0)`, still sees it. A loop without
    `add_reader` is woken by `call_soon_threadsafe` from the machine's thread, which needs the
    GIL. The frame drops its reference to `future` before the result or error leaves it: the
    error is also held natively by the future, out of the cycle collector's sight, so a
    traceback frame holding the future would keep both alive.
    """
    if not future.done():
        loop = asyncio.get_running_loop()
        woken: asyncio.Future[None] = loop.create_future()
        waker = _waker_of(loop)
        token = next(_TOKENS)
        try:
            if waker is not None:
                waker.waiting[token] = woken
                future.notify(waker.native, token)
            else:
                future.add_done_callback(_threadsafe_wake(woken))
            await woken
        except asyncio.CancelledError:
            if cancel:
                future.cancel()
            raise
        finally:
            if waker is not None:
                waker.waiting.pop(token, None)
    try:
        raw = future.result(0.0)
    finally:
        del future
    return convert(raw)


def _threadsafe_wake(woken: asyncio.Future[None]) -> Callable[[], None]:
    """A completion callback that resolves `woken` on its loop, both held weakly: the machine's
    thread drops it, and must never be the one to finalize a loop."""
    weak_woken = weakref.ref(woken)
    weak_loop = weakref.ref(woken.get_loop())

    def wake() -> None:
        loop, waiting = weak_loop(), weak_woken()
        if loop is not None and waiting is not None:
            with contextlib.suppress(RuntimeError):
                loop.call_soon_threadsafe(_resolve, waiting)

    return wake


def _resolve(woken: asyncio.Future[None]) -> None:
    if not woken.done():
        woken.set_result(None)

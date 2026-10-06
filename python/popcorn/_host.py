"""Bridge from a host request, completed on a machine's thread, to the awaiting event loop."""

from __future__ import annotations

import asyncio
import contextlib
import weakref
from collections.abc import Callable

from popcorn import _core


async def awaited[T](
    future: _core.HostFuture, convert: Callable[[object], T], *, cancel: bool = True
) -> T:
    """Await a host request on the running loop, which the request is bound to only here.

    A request already done returns without yielding. Cancelling the awaiting task cancels the
    request unless `cancel` is False: a goal halts at its next boundary, a wait is dropped, a
    command not yet run is withdrawn.

    The frame drops its reference to `future` before the result or error leaves it: the error
    is also held natively by the future, out of the cycle collector's sight, so a traceback
    frame holding the future would keep both alive. The completion callback holds the loop
    weakly: the machine's thread drops it, and must never be the one to finalize a loop.
    """
    if not future.done():
        woken: asyncio.Future[None] = asyncio.get_running_loop().create_future()
        weak_woken = weakref.ref(woken)
        weak_loop = weakref.ref(woken.get_loop())

        def wake() -> None:
            loop, waiting = weak_loop(), weak_woken()
            if loop is not None and waiting is not None:
                with contextlib.suppress(RuntimeError):
                    loop.call_soon_threadsafe(_resolve, waiting)

        future.add_done_callback(wake)
        try:
            await woken
        except asyncio.CancelledError:
            if cancel:
                future.cancel()
            raise
    try:
        raw = future.result(0.0)
    finally:
        del future
    return convert(raw)


def _resolve(woken: asyncio.Future[None]) -> None:
    if not woken.done():
        woken.set_result(None)

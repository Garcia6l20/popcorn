"""Bridge from a host request, completed on a machine's thread, to the awaiting event loop."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable

from popcorn import _core


async def awaited[T](future: _core.HostFuture, convert: Callable[[object], T]) -> T:
    """Await a host request on the running loop, which the request is bound to only here.

    Cancelling the awaiting task cancels the request: a goal halts at its next boundary, a wait
    is dropped.
    """
    loop = asyncio.get_running_loop()
    woken: asyncio.Future[None] = loop.create_future()

    def wake() -> None:
        with contextlib.suppress(RuntimeError):
            loop.call_soon_threadsafe(_resolve, woken)

    future.add_done_callback(wake)
    try:
        await woken
    except asyncio.CancelledError:
        future.cancel()
        raise
    return convert(future.result(0.0))


def _resolve(woken: asyncio.Future[None]) -> None:
    if not woken.done():
        woken.set_result(None)

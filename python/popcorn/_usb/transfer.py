from __future__ import annotations

from typing import Protocol

from popcorn import _core
from popcorn._errors import EmulationError, UsbError, UsbStall, UsbTimeout
from popcorn._streams import Waiter
from popcorn.units import VirtualTime


class _TransferLink[R](Protocol):
    def take_result(self, operation: int, /) -> R | None: ...
    def predicate_ended(self, operation: int, /) -> _core.Predicate: ...
    def forget(self, operation: int, /) -> None: ...


async def await_result[R](
    link: _TransferLink[R],
    pending: set[int],
    wait: Waiter,
    operation: int,
    posted_at: VirtualTime,
    ended_text: str,
) -> tuple[R, VirtualTime]:
    pending.add(operation)
    results: list[R] = []

    def ended() -> bool:
        if not results and operation in pending:
            taken = link.take_result(operation)
            if taken is not None:
                results.append(taken)
        return bool(results)

    done_at = posted_at
    try:
        if not ended():
            done_at = (await wait(link.predicate_ended(operation), None, None)).time
            ended()
    finally:
        pending.discard(operation)
        if not results:
            link.forget(operation)
    if not results:
        raise EmulationError(ended_text)
    return results[0], done_at


def raise_for(status: _core.UsbTransferStatus, label: str) -> None:
    if status is _core.UsbTransferStatus.completed:
        return
    message = f"{label} ended {status.name}"
    if status is _core.UsbTransferStatus.stalled:
        raise UsbStall(message, status=status.name)
    if status is _core.UsbTransferStatus.timed_out:
        raise UsbTimeout(message, status=status.name)
    raise UsbError(message, status=status.name)


def _untaken(where: str, report: int, status: _core.UsbTransferStatus | None) -> str | None:
    """The warning of an output report whose write was given up and that had failed already,
    None otherwise. One still running is warned of by the driver, from the machine's log
    queue."""
    if status is None or status is _core.UsbTransferStatus.completed:
        return None
    return f"{where}: output report {report} of a dropped write ended {status.name}"


def queue_untaken(
    future: _core.HostFuture, where: str, report: int, status: _core.UsbTransferStatus | None
) -> None:
    warning = _untaken(where, report, status)
    if warning is not None:
        future.warn_later(warning)


class UsbData(bytes):
    """Bytes a USB request returned, and the virtual time it ended at.

    Attributes:
        time: Virtual time the request ended at.
    """

    time: VirtualTime

    def __new__(cls, data: bytes, time: VirtualTime) -> UsbData:
        """Pair `data` with the time its request ended at."""
        made = super().__new__(cls, data)
        made.time = time
        return made

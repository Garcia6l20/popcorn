from __future__ import annotations

import typing
import weakref

from popcorn import _core

if typing.TYPE_CHECKING:
    from popcorn._hooks import CallbackTable


class PinWatches(dict[int, _core.PinWatch]):
    """The native pin watches of one machine by table key. They live as long as the machine,
    whether or not the device kept its `Unwatch`."""


class Unwatch:
    """A pin watch of `DeviceBoard.watch_pin` or `watch_pin_clock`: call it, or `remove()`, to end
    the watch. The watch lasts until then or until the machine is gone, whether or not this handle
    is kept. It holds no machine."""

    __slots__ = ("_key", "_table", "_watches")

    def __init__(self, watches: PinWatches, table: CallbackTable, key: int) -> None:
        """Own a handle to a watch of `watches`. `DeviceBoard.watch_pin` builds it."""
        self._watches = weakref.ref(watches)
        self._table = weakref.ref(table)
        self._key = key

    @property
    def active(self) -> bool:
        """Whether the watch is still installed."""
        watches = self._watches()
        return watches is not None and self._key in watches

    def remove(self) -> None:
        """End the watch: no edge posted before is heard. Calling it again does nothing.

        Raises:
            MachineRunning: The machine is busy, outside its callbacks.
        """
        watches = self._watches()
        if watches is None or self._key not in watches:
            return
        watches[self._key].remove()
        del watches[self._key]
        table = self._table()
        if table is not None:
            table.drop(self._key)

    def __call__(self) -> None:
        self.remove()

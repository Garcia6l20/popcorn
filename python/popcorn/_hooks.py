"""Intercepts, return hooks and watches: what their handlers see and return."""

from __future__ import annotations

import enum
import weakref
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

from popcorn import _core


class CallbackTable:
    """The Python handlers of a machine's native hooks, by key.

    Native code holds only a Relay, a weak reference to this table and a key, so that no
    reference cycle runs through native code, where the garbage collector cannot see it.
    """

    __slots__ = ("__weakref__", "_next", "_table")

    def __init__(self) -> None:
        self._next = 0
        self._table: dict[int, Callable[..., object]] = {}

    def add(self, handler: Callable[..., object]) -> int:
        """Store a handler, returning its key."""
        key = self._next
        self._next += 1
        self._table[key] = handler
        return key

    def drop(self, key: int) -> None:
        """Forget a handler."""
        self._table.pop(key, None)

    def __len__(self) -> int:
        return len(self._table)

    def __call__(self, key: int, *args: object) -> object:
        return self._table[key](*args)


class Relay:
    """What native code calls: forwards to a handler of a table while the table lives."""

    __slots__ = ("_key", "_table")

    def __init__(self, table: CallbackTable, key: int) -> None:
        self._table = weakref.ref(table)
        self._key = key

    def __call__(self, *args: object) -> object:
        table = self._table()
        if table is None:
            raise ReferenceError("the machine of this hook is gone")
        return table(self._key, *args)


class Return:
    """What an intercept handler tells the machine: run the body, skip it, or stop.

    Built by `Call.proceed()`, `Call.returns(value)` and `Call.stop(reason)`.
    """

    __slots__ = ("native", "value")

    def __init__(self, native: _core.InterceptResult, value: int | float | None = None) -> None:
        """Wrap a native result. Use the Call methods instead."""
        self.native = native
        self.value = value

    @property
    def action(self) -> str:
        """`proceed`, `returns` or `stop`."""
        return self.native.action().name


@dataclass(frozen=True, slots=True)
class Returned:
    """A call that returned, as an `on_return` handler sees it.

    Attributes:
        function: Address of the function.
        occurrence: Returns of this hook before this one.
        args: Arguments captured at entry, decoded at their C types.
        value: Return value, None for a void function.
        instructions: Instructions from entry to return, nested calls and handlers included,
            counted per translated block: a function that enters and returns within one block
            counts 0.
    """

    function: int
    occurrence: int
    args: tuple[int | float, ...]
    value: int | float | None
    instructions: int


@dataclass(slots=True)
class LogEntry:
    """One call an intercept saw: its arguments and what it returned."""

    occurrence: int
    args: tuple[int | float, ...]
    value: int | float | None = None
    returned: bool = False


class Intercept:
    """An installed intercept or return hook: `remove()` it, or use it as a context manager."""

    __slots__ = ("_hooks", "_keys", "_table", "function", "log")

    def __init__(
        self,
        hooks: Sequence[_core.Hook],
        function: int,
        log: list[LogEntry],
        table: CallbackTable,
        keys: Sequence[int],
    ) -> None:
        """Own native hooks and their handlers. `Machine.intercept` builds it."""
        self._hooks = tuple(hooks)
        self.function = function
        self.log = log
        self._table = table
        self._keys = tuple(keys)

    @property
    def active(self) -> bool:
        """Whether the hook is still installed."""
        return any(h.active() for h in self._hooks)

    def remove(self) -> None:
        """Remove the hook, from its handler too, where it goes at the end of the slice."""
        for hook in self._hooks:
            hook.remove()
        for key in self._keys:
            self._table.drop(key)

    def __enter__(self) -> Intercept:
        return self

    def __exit__(self, *_: object) -> None:
        self.remove()


class WatchKind(enum.Enum):
    """Accesses a watch reports."""

    READ = 0
    WRITE = 1
    ACCESS = 2


class WatchAction(enum.Enum):
    """What a watch handler tells the run."""

    RESUME = 0
    STOP = 1


class Initiator(enum.Enum):
    """Who made a bus access."""

    CPU = 0
    DMA = 1
    DEBUGGER = 2


@dataclass(frozen=True, slots=True)
class WatchHit:
    """One watched access.

    Attributes:
        pc: Address of the instruction that made it.
        address: First address accessed.
        size: Bytes accessed.
        value: Value read or written.
        write: Whether it was a write.
        initiator: CPU, DMA or debugger.
    """

    pc: int
    address: int
    size: int
    value: int
    write: bool
    initiator: Initiator = field(default=Initiator.CPU)


class Watch:
    """An installed watch: `remove()` it, or use it as a context manager."""

    __slots__ = ("_hook", "_key", "_table", "hits")

    def __init__(self, hook: _core.Hook, table: CallbackTable, key: int) -> None:
        """Own a native hook and its handler. `Machine.watch` builds it."""
        self._hook = hook
        self._table = table
        self._key = key
        self.hits: list[WatchHit] = []

    @property
    def active(self) -> bool:
        """Whether the watch is still installed."""
        return self._hook.active()

    def remove(self) -> None:
        """Remove the watch."""
        self._hook.remove()
        self._table.drop(self._key)

    def __enter__(self) -> Watch:
        return self

    def __exit__(self, *_: object) -> None:
        self.remove()

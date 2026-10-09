"""Intercepts, return hooks and watches: what their handlers see and return."""

from __future__ import annotations

import contextvars
import enum
import threading
import weakref
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field

from popcorn import _core


class CallbackTable:
    """The Python handlers of a machine's native hooks, by key, each with the context it runs in.

    Native code holds only a Relay, a weak reference to this table and a key, so that no
    reference cycle runs through native code, where the garbage collector cannot see it. The
    context is a copy of the one current at `add`, kept here for the same reason.
    """

    __slots__ = ("__weakref__", "_next", "_once", "_running", "_table", "closed")

    def __init__(self) -> None:
        self.closed = False
        self._next = 0
        self._table: dict[int, tuple[Callable[..., object], contextvars.Context]] = {}
        self._running: set[int] = set()
        self._once: set[int] = set()

    def add(self, handler: Callable[..., object], *, once: bool = False) -> int:
        """Store a handler and a copy of the current context, returning its key. A `once`
        handler is dropped after its first call returns."""
        key = self._next
        self._next += 1
        self._table[key] = (handler, contextvars.copy_context())
        if once:
            self._once.add(key)
        return key

    def drop(self, key: int) -> None:
        """Forget a handler."""
        self._table.pop(key, None)
        self._once.discard(key)

    def close(self) -> None:
        """Refuse every later call through a Relay, once the machine is garbage."""
        self.closed = True

    def __len__(self) -> int:
        return len(self._table)

    def __call__(self, key: int, *args: object) -> object:
        handler, context = self._table[key]
        if key in self._running:
            return handler(*args)
        self._running.add(key)
        try:
            return context.run(handler, *args)
        finally:
            self._running.discard(key)
            if key in self._once:
                self.drop(key)


class PostedBodies:
    """The one-shot table entries of posted Python bodies whose command has not run, with their
    futures, so a body whose command was withdrawn, superseded or closed is dropped from the
    caller's thread, never from native code under a host lock.

    A body that runs drops its entry itself. The others are swept at a later `add`, once the
    entries reach twice their count after the last sweep: at most twice the commands still
    queued, plus one. The lock is a leaf: only dict work runs under it.
    """

    __slots__ = ("_lock", "_swept", "_table", "_waiting")

    def __init__(self, table: CallbackTable) -> None:
        """Track the posted bodies of `table`."""
        self._table = table
        self._lock = threading.Lock()
        self._waiting: dict[int, _core.HostFuture | None] = {}
        self._swept = 0

    def add(self, body: Callable[[], object]) -> int:
        """Store `body` as a one-shot entry that forgets itself when it runs, after a sweep of
        the entries whose command ended without running them. Returns its key."""
        self._sweep()
        key: int

        def once() -> None:
            try:
                body()
            finally:
                with self._lock:
                    self._waiting.pop(key, None)

        key = self._table.add(once, once=True)
        with self._lock:
            self._waiting[key] = None
        return key

    def posted(self, key: int, future: _core.HostFuture) -> None:
        """Keep the future of the command posted for `key`, unless its body already ran."""
        with self._lock:
            if key in self._waiting:
                self._waiting[key] = future

    def drop(self, key: int) -> None:
        """Forget `key`, whose command was never posted."""
        with self._lock:
            self._waiting.pop(key, None)
        self._table.drop(key)

    def __len__(self) -> int:
        with self._lock:
            return len(self._waiting)

    def _sweep(self) -> None:
        with self._lock:
            if len(self._waiting) < max(2 * self._swept, 1):
                return
            ended = [k for k, f in self._waiting.items() if f is not None and f.done()]
            for key in ended:
                del self._waiting[key]
            self._swept = len(self._waiting)
        for key in ended:
            self._table.drop(key)


class HookGone(ReferenceError):
    """Raised by a hook whose machine's Python side was collected."""


class Relay:
    """What native code calls: forwards to a handler of a table while the table lives.

    A quiet relay returns None once the table is gone or closed, for a log sink that may outlive
    it. Another raises HookGone then.
    """

    __slots__ = ("_key", "_quiet", "_table")

    def __init__(self, table: CallbackTable, key: int, *, quiet: bool = False) -> None:
        self._table = weakref.ref(table)
        self._key = key
        self._quiet = quiet

    def __call__(self, *args: object) -> object:
        table = self._table()
        if table is None or table.closed:
            if self._quiet:
                return None
            raise HookGone("the machine of this hook is gone")
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

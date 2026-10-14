"""Intercepts, return hooks, probes and watches: what their handlers see and return."""

from __future__ import annotations

import collections
import contextvars
import enum
import itertools
import threading
import weakref
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass, field
from typing import Protocol

from popcorn import _core
from popcorn._errors import MachineRunning
from popcorn.units import VirtualDuration, VirtualTime


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


@dataclass(frozen=True, slots=True)
class Recorded[T]:
    """One recorded return.

    Attributes:
        at: Virtual time of the return.
        value: What the recording's `convert` made of the `Returned`, the `Returned` without one.
    """

    at: VirtualTime
    value: T


class RecordStore[T](Protocol):
    """Where a `Recording` keeps its entries: marks count every entry ever appended."""

    def mark(self) -> int:
        """Entries appended so far."""
        ...

    def since(self, mark: int) -> list[Recorded[T]]:
        """A copy of the entries appended at or after `mark` and still held, oldest first."""
        ...

    def dropped(self) -> int:
        """Entries dropped because the store was full."""
        ...

    def clear(self) -> None:
        """Drop every entry held, marks keep counting."""
        ...

    def stop(self) -> None:
        """Refuse every later append."""
        ...


class ReturnBuffer[T]:
    """The store of a recording with `convert`: appended on the machine's thread holding the
    GIL, under a leaf lock that no machine call, `convert` or other lock runs under."""

    __slots__ = ("_convert", "_dropped", "_entries", "_lock", "_stopped", "_total")

    def __init__(self, capacity: int, convert: Callable[[Returned], T]) -> None:
        """An empty buffer of `capacity` entries."""
        self._convert = convert
        self._entries: collections.deque[Recorded[T]] = collections.deque(maxlen=capacity)
        self._lock = threading.Lock()
        self._total = 0
        self._dropped = 0
        self._stopped = False

    def add(self, at: VirtualTime, returned: Returned) -> None:
        """Append `convert(returned)` at `at`, unless stopped before or during `convert`."""
        with self._lock:
            if self._stopped:
                return
        entry = Recorded(at, self._convert(returned))
        with self._lock:
            if self._stopped:
                return
            self._total += 1
            if len(self._entries) == self._entries.maxlen:
                self._dropped += 1
            self._entries.append(entry)

    def mark(self) -> int:
        """Entries appended so far."""
        with self._lock:
            return self._total

    def since(self, mark: int) -> list[Recorded[T]]:
        """A copy of the entries appended at or after `mark` and still held, oldest first."""
        with self._lock:
            first = self._total - len(self._entries)
            return list(itertools.islice(self._entries, max(0, mark - first), None))

    def dropped(self) -> int:
        """Entries dropped because the buffer was full."""
        with self._lock:
            return self._dropped

    def clear(self) -> None:
        """Drop every entry held, marks keep counting."""
        with self._lock:
            self._entries.clear()

    def stop(self) -> None:
        """Refuse every later append."""
        with self._lock:
            self._stopped = True


class NativeReturns:
    """The store of a recording without `convert`: a native log the machine's thread appends to
    with no GIL, decoded to `Returned` when read."""

    __slots__ = ("_decode", "_log")

    def __init__(
        self, log: _core.ReturnLog, decode: Callable[[_core.LoggedReturn], Returned]
    ) -> None:
        """Read `log`, decoding each entry with `decode`."""
        self._log = log
        self._decode = decode

    def mark(self) -> int:
        """Entries appended so far."""
        return self._log.mark()

    def since(self, mark: int) -> list[Recorded[Returned]]:
        """A decoded copy of the entries appended at or after `mark` and still held."""
        copied = self._log.since(max(0, mark))
        return [Recorded(VirtualTime(e.time_ps), self._decode(e)) for e in copied]

    def dropped(self) -> int:
        """Entries dropped because the log was full."""
        return self._log.dropped()

    def clear(self) -> None:
        """Drop every entry held, marks keep counting."""
        self._log.clear()

    def stop(self) -> None:
        """Refuse every later append."""
        self._log.stop()


class Recording[T]:
    """Returns of one function with their times, `m.record_returns`. The machine's thread
    appends, any thread reads. Holds the newest `capacity` entries, older ones are dropped and
    counted in `dropped`. Marks count every entry ever recorded, so a mark stays valid after
    drops. `remove()` stops recording and keeps the entries.

    A recording removed while its machine runs keeps the machine reachable until
    `remove_hooks()` or the machine's collection: its handler stays in the machine's callback
    table until the hook is gone."""

    __slots__ = ("_hook", "_retire", "_store")

    def __init__(self, store: RecordStore[T], hook: Intercept, retire: Callable[[], None]) -> None:
        """Read `store`, filled by `hook`, which `retire` removes from any thread.
        `Machine.record_returns` builds it."""
        self._store = store
        self._hook = hook
        self._retire = retire

    def mark(self) -> int:
        """The number of entries recorded so far, a position for `since`."""
        return self._store.mark()

    def since(self, mark: int = 0) -> list[Recorded[T]]:
        """A copy of the entries recorded at or after `mark` and still held, oldest first."""
        return self._store.since(mark)

    @property
    def dropped(self) -> int:
        """Entries dropped because the recording held `capacity` of them."""
        return self._store.dropped()

    def clear(self) -> None:
        """Drop every entry held. Marks keep counting."""
        self._store.clear()

    def remove(self) -> None:
        """Stop recording, from any thread, and keep the entries. No return is recorded once it
        returns. While the machine runs, the hook itself goes at the next block boundary, from a
        handler at the end of its slice."""
        self._store.stop()
        try:
            self._hook.remove()
        except MachineRunning:
            self._retire()

    def __enter__(self) -> Recording[T]:
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


class Probe:
    """Arrivals of the core at an address, counted natively with no handler and no GIL, and the
    time of the first. It never stops the run. `Machine.probe` builds it.

        boot = m.probe("app_entry")
        await flasher.run()
        assert await boot.hit(within=ms(2000))

    An arrival counts once, before the instruction executes, also when an intercept of the
    address then skips the function or stops the run. `remove()` it, or use it as a context
    manager. The counts stay readable after the removal, and a restore does not rewind them.
    """

    __slots__ = ("__weakref__", "_hook", "_remove", "_wait", "address")

    def __init__(
        self,
        hook: _core.ProbeHook,
        address: int,
        wait: Callable[[_core.ProbeHook, VirtualDuration], Awaitable[bool]],
        remove: Callable[[_core.ProbeHook], None],
    ) -> None:
        """Own a native probe. `Machine.probe` builds it."""
        self._hook = hook
        self._wait = wait
        self._remove = remove
        self.address = address

    @property
    def count(self) -> int:
        """Arrivals counted so far, read at any time, also while the machine runs."""
        return self._hook.count()

    @property
    def first_at(self) -> VirtualTime | None:
        """Virtual time of the first arrival, before its instruction executed, None before it.
        In `m.time`'s base at the arrival: a later reset does not move it."""
        first = self._hook.first_ps()
        return None if first is None else VirtualTime(first)

    @property
    def active(self) -> bool:
        """Whether the probe is still installed."""
        return self._hook.active()

    async def hit(self, *, within: VirtualDuration) -> bool:
        """Whether the core arrived at the address before `within` from now elapsed.

        True at once when an arrival was already counted, without waiting on the machine. A
        removed probe answers from its counts at once. Otherwise it waits up to `within`
        virtual time and answers whether the first arrival's exact time is before the start
        plus `within`. The end is exclusive: a run stopped at it has not executed the
        instruction there, so an arrival exactly at the end answers False wherever the block
        boundaries fall, even when it was counted. Parked, the start is `m.time`, and it drives
        the run as `wait_until` does, stopping at the first event boundary or slice end after
        the arrival or when `within` elapsed. Inside a free run, `start()` or `running()`, it
        waits on the free run and the start is the block boundary that registers the wait, as
        `wait_until`'s `for_`: the wake may come up to one slice after the arrival, the answer
        does not depend on it.
        Cancelling it while parked halts the run, inside a free run it drops the wait only.

        Returns:
            Whether the first arrival came within the time.

        Raises:
            TypeError: `within` is no `VirtualDuration`.
            ValueError: `within` is negative.
            MachineBusy: A run, step, call or wait is active outside a free run, or the machine
                is a member of a linked system that runs.
            RuntimeError: Called from a handler, a model, a predicate or a posted body.
            EmulationError: The run stopped for another reason: FaultError for FAULT,
                BudgetExceeded for BUDGET, EmulationError otherwise, USER included when an
                intercept's `call.stop()` stopped a parked run. Inside a free run, the stop
                that ended it.
            BaseException: What a handler of the free run raised.
        """
        if not isinstance(within, VirtualDuration):  # pyright: ignore[reportUnnecessaryIsInstance]
            raise TypeError("within is a VirtualDuration, a span of virtual time")
        if within.ps < 0:
            raise ValueError(f"within must not be negative, got {within}")
        if self._hook.count() > 0 or not self._hook.active():
            return self._hook.count() > 0
        return await self._wait(self._hook, within)

    def remove(self) -> None:
        """Remove the probe: at once while parked, at the next block boundary while the machine
        runs, this call waiting for it there. Arrivals after it do not count. While the machine
        runs, `active` turns False at the call, and a KeyboardInterrupt that ends the wait still
        removes the probe at the next boundary.

        Raises:
            MachineRunning: Called from a handler of this machine during a slice, or the machine
                is a member of a linked system that runs. The probe stays installed.
            RuntimeError: Called from a callback of another machine.
        """
        self._remove(self._hook)

    def __enter__(self) -> Probe:
        return self

    def __exit__(self, *_: object) -> None:
        self.remove()

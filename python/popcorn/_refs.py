"""References to machine state for `Machine.read` and `Machine.write`, and their results."""

from __future__ import annotations

import asyncio
import enum
import operator
import struct
from collections.abc import Callable, Coroutine, Generator
from types import TracebackType
from typing import TYPE_CHECKING, Any, Literal, cast, override

from popcorn import _core
from popcorn._enums import PinMode
from popcorn._host import awaited
from popcorn._views import microvolts_of
from popcorn.units import VirtualTime

if TYPE_CHECKING:
    from popcorn._dwarf_views import ScalarLayout, Var
    from popcorn._streams import UartStream
    from popcorn._usb import UsbHost, UsbRequest, UsbState
    from popcorn._views import (
        AnalogInput,
        Memory,
        Peripherals,
        PeripheralView,
        Pin,
        RegisterBank,
        RegisterView,
        ScalarView,
    )


class Readout[*Ts](tuple[*Ts]):
    """Values of one access batch, in argument order, and the virtual time it executed at.

    A tuple: `r0, word = await m.read(...)` unpacks it, and it equals the plain tuple of its
    values.

    Attributes:
        time: Virtual time of the block boundary the batch executed at.
    """

    time: VirtualTime

    def __new__(cls, values: tuple[*Ts], time: VirtualTime) -> Readout[*Ts]:
        """Pair `values` with the time they were read at."""
        made = super().__new__(cls, values)
        made.time = time
        return made

    @override
    def __repr__(self) -> str:
        return f"Readout({tuple(self)!r}, time={self.time})"


_THROWN = "a command whose await was thrown into failed later"
_CANCELLED = "a command its awaiter cancelled failed later"
_AWAIT_FAILED = "a command whose await failed before its result failed later"
_DROPPED = "a command whose Pending was dropped failed later"


class Pending[R](Coroutine[Any, Any, R]):
    """A command posted to a machine. Await it for its result.

    The command is posted when the method returning it is called, not when it is awaited: a
    stamped command waits for its time even while nothing awaits it. A coroutine, so
    `asyncio.run` and `asyncio.create_task` take it. Cancelling the awaiting task withdraws a
    command not yet run. A command whose exception is never retrieved reports it to
    `sys.unraisablehook` when the Pending is collected, or, still running then, when it fails
    later, at the machine's next log flush, except a failure because the machine closed: a
    stamped command left queued when a script ends is no error.

    A command a cancel leaves in place, `HidStream.write_report`, spends its Pending: a later
    failure of the command goes to `sys.unraisablehook` as "a command its awaiter cancelled
    failed later" at the machine's next log flush, and awaiting the Pending again raises
    RuntimeError. An exception other than a cancel thrown into the await, which asyncio never
    does, leaves the command in place the same way, reported as "a command whose await was thrown
    into failed later", or, for an error the await raised itself, "a command whose await failed
    before its result failed later".
    """

    __slots__ = (
        "__weakref__",
        "_cancel",
        "_convert",
        "_dropped",
        "_finished",
        "_follow",
        "_future",
        "_inner",
        "_spent",
        "_thrown",
    )

    def __init__(
        self,
        future: _core.HostFuture,
        convert: Callable[[object], Any],
        *,
        cancel: bool = True,
        follow: Callable[[Any, _core.HostFuture], Coroutine[Any, Any, R]] | None = None,
        dropped: Callable[[_core.HostFuture], None] | None = None,
    ) -> None:
        """Wrap a posted command. `follow` continues from its result and its future when first
        awaited, and warns through `HostFuture.warn_later` as `dropped` does. `cancel` False
        leaves the command in place when the awaiting task is cancelled.
        `dropped` gets the future on the caller's thread when the Pending is collected unawaited
        or its await leaves the command in place, to release what `follow` would have. It must
        release natively, with no continuation on the machine's thread, and warn through
        `HostFuture.warn_later`: a collection may run it on a machine's thread."""
        self._future: _core.HostFuture | None = future
        self._convert = convert
        self._cancel = cancel
        self._follow = follow
        self._dropped = dropped
        self._inner: Coroutine[Any, Any, R] | None = None
        self._spent = False
        self._finished = False
        self._thrown = False

    def done(self) -> bool:
        """Whether the command ran, or failed."""
        return self._finished or (self._future is not None and self._future.done())

    def mapped[S](self, convert: Callable[[R], S]) -> Pending[S]:
        """The same command with its result passed through `convert`. This one is spent."""
        future = self._future
        if self._follow is not None or self._inner is not None or future is None:
            raise RuntimeError("only a fresh command without a continuation can be mapped")
        self._spent = True
        first = self._convert
        return Pending(future, lambda raw: convert(first(raw)), cancel=self._cancel)

    @override
    def __await__(self) -> Generator[Any, None, R]:
        return self._coroutine().__await__()

    @override
    def send(self, value: Any, /) -> Any:
        return self._coroutine().send(value)

    @override
    def throw(
        self,
        typ: type[BaseException] | BaseException,
        val: object = None,
        tb: TracebackType | None = None,
        /,
    ) -> Any:
        error = typ if isinstance(typ, BaseException) else typ() if val is None else typ(val)
        self._thrown = True
        if self._inner is None:
            if isinstance(error, asyncio.CancelledError):
                self._cancelled_unstarted()
            elif self._future is not None:
                self._leave(self._future, _THROWN)
        return self._coroutine().throw(error)

    def _cancelled_unstarted(self) -> None:
        """Withdraw or leave the command as a cancel of a started await does, for a task cancelled
        before its first step, which throws into the await before it runs."""
        future = self._future
        if future is None:
            return
        if self._cancel:
            self._future = None
            self._finished = True
            future.report_late_failure(_CANCELLED, True, future.cancel())
        else:
            self._leave(future, _CANCELLED)

    def _leave(self, future: _core.HostFuture, message: str) -> None:
        """Leave the command in place, its later failure reported with `message`."""
        future.report_late_failure(message, False, False)
        dropped, self._dropped = self._dropped, None
        if dropped is not None:
            dropped(future)

    @override
    def close(self) -> None:
        if self._inner is not None:
            self._inner.close()

    def _coroutine(self) -> Coroutine[Any, Any, R]:
        if self._inner is None:
            self._inner = self._result()
        return self._inner

    async def _result(self) -> R:
        future = self._future
        if future is None:
            raise RuntimeError("this command was already awaited")
        left_in_place = False
        try:
            value = await awaited(future, self._convert, cancel=False)
            ran = future
        except asyncio.CancelledError, GeneratorExit:
            left_in_place = not self._cancel
            if left_in_place:
                self._leave(future, _CANCELLED)
            else:
                future.report_late_failure(_CANCELLED, True, future.cancel())
            raise
        except Exception:
            left_in_place = not future.done()
            if left_in_place:
                self._leave(future, _THROWN if self._thrown else _AWAIT_FAILED)
            raise
        finally:
            if not left_in_place:
                self._future = None
                self._finished = True
            del future
        if self._follow is not None:
            return await self._follow(value, ran)
        result: R = value
        return result

    def __del__(self) -> None:
        future = self._future
        if self._spent or self._inner is not None or future is None:
            return
        self._future = None
        future.report_late_failure(_DROPPED, True, False)
        dropped, self._dropped = self._dropped, None
        if dropped is not None:
            dropped(future)


def _same(raw: object) -> Any:
    return raw


class Ref[T]:
    """A location of one machine's state, `m.ref.reg.r0`. It never touches the machine:
    `Machine.read` and `Machine.write` access it, as one command of native ops that run with no
    GIL, the bodies the sync views run."""

    __slots__ = ("_check", "_decode", "_get", "_read", "_set", "_write", "label", "owner")

    def __init__(
        self,
        owner: object,
        label: str,
        get: Callable[[], T],
        put: Callable[[T], None] | None = None,
        check: Callable[[object], None] | None = None,
        *,
        read: Callable[[], _core.AccessOp],
        decode: Callable[[object], T] = _same,
        write: Callable[[T], _core.AccessOp] | None = None,
    ) -> None:
        """A reference. Use the tree of `Machine.ref` instead."""
        self.owner = owner
        self.label = label
        self._get = get
        self._set = put
        self._check = check
        self._read = read
        self._decode = decode
        self._write = write

    @property
    def writable(self) -> bool:
        """Whether `Machine.write` takes it."""
        return self._set is not None

    def checked(self, value: object) -> None:
        """Raise what the sync setter raises for `value` before anything is posted.

        Raises:
            TypeError: The reference is read-only, or `value` has the wrong type.
            ValueError: `value` is out of range.
        """
        if self._set is None:
            raise TypeError(f"{self.label} is read-only")
        if self._check is not None:
            self._check(value)

    def get(self) -> T:
        """Read the location now, as the sync view does."""
        return self._get()

    def put(self, value: T) -> None:
        """Write the location now, as the sync view does."""
        if self._set is None:
            raise TypeError(f"{self.label} is read-only")
        self._set(value)

    def read_op(self) -> _core.AccessOp:
        """The native op `Machine.read` runs, its raw value decoded by `decode`."""
        return self._read()

    def decode(self, raw: object) -> T:
        """The value of the raw result of `read_op`."""
        return self._decode(raw)

    def write_op(self, value: T) -> _core.AccessOp:
        """The native op `Machine.write` runs for `value`, encoded and checked now.

        Raises:
            TypeError: The reference is read-only, or `value` has the wrong type.
            ValueError: `value` is out of range.
        """
        self.checked(value)
        if self._write is None:
            raise TypeError(f"{self.label} has no native write")
        return self._write(value)

    @override
    def __repr__(self) -> str:
        return f"Ref({self.label})"


def _integer(bits: int, label: str) -> Callable[[object], None]:
    def check(value: object) -> None:
        if isinstance(value, bool) or not isinstance(value, int):
            raise TypeError(f"{label} takes an int, not {type(value).__name__}")
        if value < 0 or value >> bits:
            raise ValueError(f"{value} does not fit the {bits}-bit {label}")

    return check


def _handle(owner: object) -> _core.MachineHandle:
    return cast(_core.MachineHandle, owner)


def _packs(fmt: str) -> Callable[[object], None]:
    def check(value: object) -> None:
        try:
            struct.pack(fmt, value)
        except (struct.error, OverflowError) as error:
            raise ValueError(f"{value!r} does not fit {fmt[1:]}: {error}") from error

    return check


type Comparison = Literal["eq", "ne", "lt", "le", "gt", "ge"]

_COMPARE: dict[Comparison, Callable[[Any, Any], bool]] = {
    "eq": operator.eq,
    "ne": operator.ne,
    "lt": operator.lt,
    "le": operator.le,
    "gt": operator.gt,
    "ge": operator.ge,
}


class Condition:
    """A comparison of one plain-memory scalar against a constant, built by `MemoryRef.eq` and
    its siblings. `Machine.wait_for` takes it as an `Event`.

    Inside a free run it is evaluated natively, no Python, after every dispatched event and at
    slice ends: a wait on it ends at the first event boundary or slice end after the value
    changed, the boundary a parked `run(stop_when=)` sees. Parked, `wait_for` runs its `ready`
    as a `stop_when`.

        link_up = m.ref.var("link").up
        async with m.running():
            stop = await m.wait_for(link_up.eq(True), for_=s(4))
        assert stop.fired, stop
    """

    __slots__ = ("_label", "_native", "_ref", "_test")

    def __init__(
        self,
        native: _core.MemoryCondition,
        ref: MemoryRef[Any],
        test: Callable[[Any], bool],
        label: str,
    ) -> None:
        """A condition. Use the methods of `MemoryRef` instead."""
        self._native = native
        self._ref = ref
        self._test = test
        self._label = label

    @property
    def label(self) -> str:
        """The reference's label and the comparison, as `repr` shows them. Read-only."""
        return self._label

    def ready(self, machine: object) -> bool:
        """Evaluate it now through the reference's sync view: the parked path."""
        return self._test(self._ref.get())

    @override
    def __repr__(self) -> str:
        return f"Condition({self.label})"


def native_of(condition: Condition) -> _core.MemoryCondition:
    """The native form of `condition`."""
    return condition._native  # pyright: ignore[reportPrivateUsage]


def owner_of(event: Condition | Conditions) -> object:
    """The machine handle whose memory `event` reads."""
    first = event.members[0] if isinstance(event, Conditions) else event
    return first._ref.owner  # pyright: ignore[reportPrivateUsage]


def native_set(conditions: Conditions) -> _core.ConditionSet:
    """The native form of `conditions`."""
    native = _core.ConditionSet()
    native.combine = (
        _core.ConditionMode.all if conditions.mode == "all" else _core.ConditionMode.any
    )
    native.members = [native_of(member) for member in conditions.members]
    return native


class Conditions:
    """Conditions evaluated natively together after every event of a free run, as one
    `Machine.wait_for` event. Flat and immutable: build it with `any_of` or `all_of`.

        up, failed = m.ref.var("link").up, m.ref.var("link").error
        either = popcorn.any_of(up.eq(True), failed.ne(0))
        async with m.running():
            stop = await m.wait_for(either, for_=s(4))
        assert stop.fired == (either.members[0],), stop.fired

    The stop's `fired` names the members that held at the boundary that ended the wait, in the
    order given: every member for `all_of`, each member true there for `any_of`.

    Attributes:
        mode: "any" for `any_of`, "all" for `all_of`.
        members: The conditions, in the order given.
    """

    __slots__ = ("members", "mode")

    mode: Literal["any", "all"]
    members: tuple[Condition, ...]

    def __init__(self, mode: Literal["any", "all"], members: tuple[Condition, ...]) -> None:
        """A set. Use `any_of` or `all_of` instead."""
        object.__setattr__(self, "mode", mode)
        object.__setattr__(self, "members", members)

    @override
    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError(f"Conditions is immutable, {name} cannot be set")

    @override
    def __delattr__(self, name: str) -> None:
        raise AttributeError(f"Conditions is immutable, {name} cannot be deleted")

    def ready(self, machine: object) -> bool:
        """Evaluate the members now through their sync views: the parked path."""
        combine = all if self.mode == "all" else any
        return combine(member.ready(machine) for member in self.members)

    @override
    def __repr__(self) -> str:
        return f"{self.mode}_of({', '.join(member.label for member in self.members)})"


def _combined(mode: Literal["any", "all"], conditions: tuple[object, ...]) -> Conditions:
    for member in conditions:
        if not isinstance(member, Condition):
            raise TypeError(
                f"{mode}_of takes Condition members only, not {type(member).__name__}: sets are"
                " flat"
            )
    members = cast(tuple[Condition, ...], conditions)
    if not 1 <= len(members) <= 64:
        raise ValueError(f"{mode}_of takes 1 to 64 conditions, not {len(members)}")
    if any(owner_of(member) is not owner_of(members[0]) for member in members):
        raise ValueError(f"{mode}_of takes conditions of one machine")
    return Conditions(mode, members)


def any_of(*conditions: Condition) -> Conditions:
    """Ready once at least one member holds.

    Raises:
        TypeError: A member is not a `Condition`, a `Conditions` included.
        ValueError: No member, more than 64, or members of different machines.
    """
    return _combined("any", conditions)


def all_of(*conditions: Condition) -> Conditions:
    """Ready once every member holds at the same boundary. Same errors as `any_of`."""
    return _combined("all", conditions)


class MemoryRef[T: (int, float, bool)](Ref[T]):
    """A reference to a scalar in plain memory, RAM, flash or backup SRAM: `m.ref.mem.u32[a]`
    and a scalar `m.ref.var(...)`. Builds conditions for `Machine.wait_for`.

    Each comparison method raises when the condition cannot be evaluated natively:

    Raises:
        TypeError: The reference is not a scalar, spans more than 8 bytes, or the constant has
            the wrong type. A bool takes `eq` and `ne` only.
        ValueError: The scalar is not in plain memory: a peripheral window or an unmapped
            address. Or the constant is out of the scalar's range. Or an int constant of a
            float scalar is not exactly a double: both paths compare in double, so
            `f64.lt(2**53 + 1)` is refused.
    """

    __slots__ = ("_locate", "_mask")

    def __init__(
        self,
        owner: object,
        label: str,
        get: Callable[[], T],
        put: Callable[[T], None] | None,
        check: Callable[[object], None] | None,
        locate: Callable[[], ScalarLayout],
        mask: int | None = None,
        *,
        read: Callable[[], _core.AccessOp],
        decode: Callable[[object], T],
        write: Callable[[T], _core.AccessOp] | None = None,
    ) -> None:
        """A memory reference. Use the tree of `Machine.ref` instead."""
        super().__init__(owner, label, get, put, check, read=read, decode=decode, write=write)
        self._locate = locate
        self._mask = mask

    def eq(self, value: T) -> Condition:
        """The scalar equals `value`."""
        return self._condition("eq", value)

    def ne(self, value: T) -> Condition:
        """The scalar differs from `value`."""
        return self._condition("ne", value)

    def lt(self, value: T) -> Condition:
        """The scalar is below `value`."""
        return self._condition("lt", value)

    def le(self, value: T) -> Condition:
        """The scalar is at most `value`."""
        return self._condition("le", value)

    def gt(self, value: T) -> Condition:
        """The scalar is above `value`."""
        return self._condition("gt", value)

    def ge(self, value: T) -> Condition:
        """The scalar is at least `value`."""
        return self._condition("ge", value)

    def masked(self, mask: int) -> MemoryRef[int]:
        """The integer scalar ANDed with `mask`, compared unsigned. A signed scalar is taken as
        its two's complement bits: an `i8` holding -1 masked with 0xFF is 255. Read-only.

        Raises:
            TypeError: The scalar is not an integer, or `mask` is not an int.
            ValueError: `mask` is negative or wider than the scalar.
        """
        if type(mask) is not int:
            raise TypeError(f"a mask is an int, not {type(mask).__name__}")
        layout = self._layout()
        if layout.kind not in ("unsigned", "signed"):
            raise TypeError(f"{self.label} is not an integer, it takes no mask")
        if mask < 0 or mask >> layout.bits:
            raise ValueError(f"mask {mask:#x} does not fit the {layout.bits}-bit {self.label}")
        combined = mask if self._mask is None else self._mask & mask
        get = self._get
        decode = self._decode
        return MemoryRef[int](
            self.owner,
            f"{self.label}.masked({mask:#x})",
            lambda: int(get()) & combined,
            None,
            None,
            self._locate,
            combined,
            read=self._read,
            decode=lambda raw: int(decode(raw)) & combined,
        )

    def _layout(self) -> ScalarLayout:
        layout = self._locate()
        if layout.size > 8:
            raise TypeError(f"{self.label} spans {layout.size} bytes, a condition reads at most 8")
        return layout

    def _condition(self, compare: Comparison, value: object) -> Condition:
        layout = self._layout()
        handle = cast(_core.MachineHandle, self.owner)
        if not handle.is_plain_memory(layout.address, layout.size):
            raise ValueError(
                f"{self.label} at {layout.address:#010x} is not plain memory: a condition reads"
                " RAM, flash or backup SRAM, not a peripheral window or an unmapped address"
            )
        native = _core.MemoryCondition()
        native.where = layout.address
        native.size = layout.size
        native.shift = layout.shift
        native.bits = layout.bits
        op = compare
        if self._mask is not None:
            self._integer(value, 0, (1 << layout.bits) - 1)
            native.masked = True
            native.mask = self._mask
            native.encoding = _core.ConditionKind.unsigned_int
            native.value = cast(int, value)
        elif layout.kind == "bool":
            if compare not in ("eq", "ne"):
                raise TypeError(f"{self.label} is a bool, it takes eq and ne only")
            if not isinstance(value, bool):
                raise TypeError(f"{self.label} is a bool, compare it to a bool")
            op = "ne" if (compare == "eq") == value else "eq"
            native.encoding = _core.ConditionKind.unsigned_int
        elif layout.kind == "float":
            if isinstance(value, bool) or not isinstance(value, int | float):
                raise TypeError(f"{self.label} is a float, not {type(value).__name__}")
            if isinstance(value, int) and not _exact_double(value):
                raise ValueError(
                    f"{_shortened(value)} is not exactly a double, {self.label} compares in double"
                )
            native.encoding = _core.ConditionKind.floating
            native.real = float(value)
        elif layout.kind == "signed":
            half = 1 << (layout.bits - 1)
            self._integer(value, -half, half - 1)
            native.encoding = _core.ConditionKind.signed_int
            native.value = cast(int, value) & ((1 << 64) - 1)
        else:
            if layout.bits == 1 and isinstance(value, bool):
                value = int(value)
            self._integer(value, 0, (1 << layout.bits) - 1)
            native.encoding = _core.ConditionKind.unsigned_int
            native.value = cast(int, value)
        native.compare = getattr(_core.ConditionOp, op)
        test = _COMPARE[compare]
        return Condition(
            native, self, lambda current: test(current, value), f"{self.label}.{compare}({value!r})"
        )

    def _integer(self, value: object, low: int, high: int) -> None:
        if isinstance(value, bool) or not isinstance(value, int):
            raise TypeError(f"{self.label} is an integer, not {type(value).__name__}")
        if not low <= value <= high:
            raise ValueError(f"{value} is outside {self.label}, {low} to {high}")


def _shortened(value: int) -> str:
    text = str(value)
    if len(text) <= 24:
        return text
    return f"{text[:6]}...{text[-3:]} ({len(text.lstrip('-'))} digits)"


def _exact_double(value: int) -> bool:
    try:
        return float(value) == value
    except OverflowError:
        return False


def _memory_layout(address: int, fmt: str) -> ScalarLayout:
    from popcorn._dwarf_views import ScalarLayout

    code = _FORMATS[fmt]
    size = struct.calcsize(code)
    kind: Literal["unsigned", "signed", "float"] = (
        "float" if code in "fd" else "signed" if code.islower() else "unsigned"
    )
    return ScalarLayout(address, size, 0, 8 * size, kind)


class ScalarRefs[T: (int, float)]:
    """References to little-endian scalars of one type, `m.ref.mem.u32[addr]`."""

    __slots__ = ("_fmt", "_owner", "_view")

    def __init__(self, owner: object, view: ScalarView[T], fmt: str) -> None:
        """Wrap a scalar view. Use `m.ref.mem.<fmt>` instead."""
        self._owner = owner
        self._view = view
        self._fmt = fmt

    def __getitem__(self, address: int) -> MemoryRef[T]:
        view = self._view
        fmt = self._fmt
        packing = "<" + _FORMATS[fmt]
        size = struct.calcsize(packing)
        handle = _handle(self._owner)

        def put(value: T) -> None:
            view[address] = value

        def decode(raw: object) -> T:
            value: T = struct.unpack(packing, cast(bytes, raw))[0]
            return value

        return MemoryRef(
            self._owner,
            f"mem.{fmt}[{address:#010x}]",
            lambda: view[address],
            put,
            _packs(packing),
            lambda: _memory_layout(address, fmt),
            read=lambda: handle.op_read_memory(address, size),
            decode=decode,
            write=lambda value: handle.op_write_memory(address, struct.pack(packing, value)),
        )


_FORMATS = {
    "u8": "B",
    "u16": "H",
    "u32": "I",
    "u64": "Q",
    "i8": "b",
    "i16": "h",
    "i32": "i",
    "i64": "q",
    "f32": "f",
    "f64": "d",
}


class MemoryRefs:
    """References to guest memory, `m.ref.mem`."""

    __slots__ = (
        "_memory",
        "_owner",
        "f32",
        "f64",
        "i8",
        "i16",
        "i32",
        "i64",
        "u8",
        "u16",
        "u32",
        "u64",
    )

    def __init__(self, owner: object, memory: Memory) -> None:
        """Wrap the memory view. Use `m.ref.mem` instead."""
        self._owner = owner
        self._memory = memory
        self.u8 = ScalarRefs[int](owner, memory.u8, "u8")
        self.u16 = ScalarRefs[int](owner, memory.u16, "u16")
        self.u32 = ScalarRefs[int](owner, memory.u32, "u32")
        self.u64 = ScalarRefs[int](owner, memory.u64, "u64")
        self.i8 = ScalarRefs[int](owner, memory.i8, "i8")
        self.i16 = ScalarRefs[int](owner, memory.i16, "i16")
        self.i32 = ScalarRefs[int](owner, memory.i32, "i32")
        self.i64 = ScalarRefs[int](owner, memory.i64, "i64")
        self.f32 = ScalarRefs[float](owner, memory.f32, "f32")
        self.f64 = ScalarRefs[float](owner, memory.f64, "f64")

    def block(self, address: int, size: int) -> Ref[bytes]:
        """`size` bytes at `address`, `Memory.read` and `write`. A write takes exactly `size`
        bytes."""
        memory = self._memory

        def check(value: object) -> None:
            if not isinstance(value, bytes | bytearray):
                raise TypeError(f"a memory block takes bytes, not {type(value).__name__}")
            if len(value) != size:
                raise ValueError(
                    f"the block at {address:#010x} takes {size} bytes, not {len(value)}"
                )

        handle = _handle(self._owner)
        return Ref(
            self._owner,
            f"mem.block({address:#010x}, {size})",
            lambda: memory.read(address, size),
            lambda data: memory.write(address, bytes(data)),
            check,
            read=lambda: handle.op_read_memory(address, size),
            decode=lambda raw: bytes(cast(bytes, raw)),
            write=lambda data: handle.op_write_memory(address, bytes(data)),
        )

    def cstring(self, address: int, limit: int = 256) -> Ref[str]:
        """The NUL-terminated string at `address`, `Memory.cstring`. Read-only."""
        memory = self._memory
        handle = _handle(self._owner)
        return Ref(
            self._owner,
            f"mem.cstring({address:#010x})",
            lambda: memory.cstring(address, limit),
            read=lambda: handle.op_cstring(address, limit),
            decode=lambda raw: cast(bytes, raw).decode("utf-8", errors="replace"),
        )


class RegisterRefs:
    """References to core registers, `m.ref.reg.r0` or `m.ref.reg[Reg.R0]`."""

    __slots__ = ("_bank", "_owner")

    def __init__(self, owner: object, bank: RegisterBank[Any]) -> None:
        """Wrap the register bank. Use `m.ref.reg` instead."""
        self._owner = owner
        self._bank = bank

    def __getattr__(self, name: str) -> Ref[int]:
        bank = self._bank
        if name.startswith("_") or name not in bank.names():
            raise AttributeError(f"this core has no register {name}")

        def put(value: int) -> None:
            setattr(bank, name, value)

        def get() -> int:
            value: int = getattr(bank, name)
            return value

        index = bank.names().index(name)
        return _register_ref(self._owner, f"reg.{name}", index, get, put)

    def __getitem__(self, register: enum.Enum) -> Ref[int]:
        bank = self._bank
        index = bank._slot(register)  # pyright: ignore[reportPrivateUsage]

        def put(value: int) -> None:
            bank[register] = value

        return _register_ref(
            self._owner, f"reg[{register.value}]", index, lambda: bank[register], put
        )


def _register_ref(
    owner: object, label: str, index: int, get: Callable[[], int], put: Callable[[int], None]
) -> Ref[int]:
    handle = _handle(owner)
    return Ref(
        owner,
        label,
        get,
        put,
        _integer(64, "register"),
        read=lambda: handle.op_read_register(index),
        write=lambda value: handle.op_write_register(index, value),
    )


class RegisterRef(Ref[int]):
    """A peripheral register, `m.ref.periph.USART1.CR1`, as `RegisterView.value`: a read is
    the stored value, a write goes over the bus as the debugger. A field by name, `.UE`, writes
    the field with its siblings neutral, as `RegisterView.FIELD = v`. `.bus` reads over the bus
    too."""

    __slots__ = ("_view",)

    def __init__(self, owner: object, label: str, view: RegisterView) -> None:
        """Wrap a register view. Use `m.ref.periph.<P>.<R>` instead."""
        width = view._width  # pyright: ignore[reportPrivateUsage]
        peripheral = view._peripheral  # pyright: ignore[reportPrivateUsage]
        offset = view._offset  # pyright: ignore[reportPrivateUsage]
        handle = _handle(owner)
        full = (1 << width) - 1

        def put(value: int) -> None:
            view.value = value

        super().__init__(
            owner,
            label,
            lambda: view.value,
            put,
            _integer(width, "register"),
            read=lambda: handle.op_peek(peripheral, offset, width),
            write=lambda value: handle.op_write_field(peripheral, offset, width, full, value),
        )
        self._view = view

    @property
    def bus(self) -> Ref[int]:
        """The register over the bus, `RegisterView.read` and `write`."""
        view = self._view
        width = view._width  # pyright: ignore[reportPrivateUsage]
        at = view._base + view._offset  # pyright: ignore[reportPrivateUsage]
        size = width // 8
        handle = _handle(self.owner)
        return Ref(
            self.owner,
            f"{self.label}.bus",
            view.read,
            view.write,
            _integer(width, "register"),
            read=lambda: handle.op_read_memory(at, size),
            decode=lambda raw: int.from_bytes(cast(bytes, raw), "little"),
            write=lambda value: handle.op_write_memory(at, value.to_bytes(size, "little")),
        )

    def __getattr__(self, name: str) -> Ref[int]:
        view = self._view
        field = view._field(name)  # pyright: ignore[reportPrivateUsage]

        def put(value: int) -> None:
            setattr(view, name, value)

        def get() -> int:
            value: int = getattr(view, name)
            return value

        width = view._width  # pyright: ignore[reportPrivateUsage]
        peripheral = view._peripheral  # pyright: ignore[reportPrivateUsage]
        offset = view._offset  # pyright: ignore[reportPrivateUsage]
        handle = _handle(self.owner)
        return Ref(
            self.owner,
            f"{self.label}.{name}",
            get,
            put,
            _integer(field.width, "field"),
            read=lambda: handle.op_peek(peripheral, offset, width),
            decode=lambda raw: (cast(int, raw) & field.mask) >> field.lsb,
            write=lambda value: handle.op_write_field(
                peripheral, offset, width, field.mask, value << field.lsb
            ),
        )


class PinRefs:
    """References to one GPIO pin, `m.ref.periph.GPIOA.pin(0)`."""

    __slots__ = ("_label", "_owner", "_pin")

    def __init__(self, owner: object, label: str, pin: Pin) -> None:
        """Wrap a pin. Use `m.ref.periph.<GPIOx>.pin(n)` instead."""
        self._owner = owner
        self._label = label
        self._pin = pin

    @property
    def level(self) -> Ref[bool]:
        """Level as the input data register reads it. A write drives the pin, None releases
        it."""
        pin = self._pin
        handle = _handle(self._owner)

        def put(value: bool | None) -> None:
            if value is None:
                pin.release()
            else:
                pin.level = value

        def write(value: bool | None) -> _core.AccessOp:
            return handle.op_gpio_drive(
                pin.port, pin.number, None if value is None else bool(value)
            )

        return Ref[bool](
            self._owner,
            f"{self._label}.level",
            lambda: pin.level,
            put,
            read=lambda: handle.op_gpio_level(pin.port, pin.number),
            decode=lambda raw: bool(raw),
            write=write,
        )

    @property
    def mode(self) -> Ref[PinMode]:
        """Mode the firmware configured. Read-only."""
        pin = self._pin
        handle = _handle(self._owner)
        return Ref(
            self._owner,
            f"{self._label}.mode",
            lambda: pin.mode,
            read=lambda: handle.op_gpio_mode(pin.port, pin.number),
            decode=lambda raw: PinMode(cast(int, raw)),
        )

    @property
    def alternate(self) -> Ref[int]:
        """Alternate function number. Read-only."""
        pin = self._pin
        handle = _handle(self._owner)
        return Ref(
            self._owner,
            f"{self._label}.alternate",
            lambda: pin.alternate,
            read=lambda: handle.op_gpio_alternate(pin.port, pin.number),
        )


class AnalogRefs:
    """References to one external ADC channel, `m.ref.periph.ADC1.input(5)`."""

    __slots__ = ("_input", "_label", "_owner")

    def __init__(self, owner: object, label: str, analog: AnalogInput) -> None:
        """Wrap a channel. Use `m.ref.periph.<ADCx>.input(n)` instead."""
        self._owner = owner
        self._label = label
        self._input = analog

    @property
    def volts(self) -> Ref[float]:
        """Voltage driven on the channel."""
        analog = self._input

        def put(value: float) -> None:
            analog.volts = value

        def check(value: object) -> None:
            if isinstance(value, bool) or not isinstance(value, int | float):
                raise TypeError(f"volts takes a float, not {type(value).__name__}")
            microvolts_of(value)

        handle = _handle(self._owner)
        return Ref(
            self._owner,
            f"{self._label}.volts",
            lambda: analog.volts,
            put,
            check,
            read=lambda: handle.op_adc_input(analog.adc, analog.channel),
            decode=lambda raw: cast(int, raw) / 1e6,
            write=lambda value: handle.op_adc_drive(
                analog.adc, analog.channel, microvolts_of(value)
            ),
        )


class AudioRefs:
    """References to one SAI audio block, `m.ref.periph.SAI1.audio("B")`."""

    __slots__ = ("_label", "_owner", "_stream")

    def __init__(self, owner: object, label: str, stream: Any) -> None:
        """Wrap a stream. Use `m.ref.periph.<SAI>.audio(block)` instead."""
        self._owner = owner
        self._label = label
        self._stream = stream

    @property
    def external_frame_hz(self) -> Ref[int]:
        """Frame rate of the external clock master, `AudioStream.external_frame_hz`."""
        stream = self._stream

        def put(value: int) -> None:
            stream.external_frame_hz = value

        def get() -> int:
            value: int = stream.external_frame_hz
            return value

        link = stream._link  # pyright: ignore[reportPrivateUsage]
        return Ref(
            self._owner,
            f"{self._label}.external_frame_hz",
            get,
            put,
            _integer(32, "frame rate"),
            read=link.op_external_frame_hz,
            write=link.op_set_external_frame_hz,
        )


class PortRefs:
    """References to one UART stream, `m.ref.periph.USART1.port`."""

    __slots__ = ("_label", "_owner", "_stream")

    def __init__(self, owner: object, label: str, stream: UartStream) -> None:
        """Wrap a stream. Use `m.ref.periph.<UART>.port` instead."""
        self._owner = owner
        self._label = label
        self._stream = stream

    @property
    def queued(self) -> Ref[int]:
        """Bytes written and not yet delivered to the firmware's receiver. Read-only."""
        stream = self._stream
        return Ref(
            self._owner,
            f"{self._label}.queued",
            lambda: stream.queued,
            read=stream._link.op_queued,  # pyright: ignore[reportPrivateUsage]
        )


class HostRefs:
    """References to a virtual USB host, `m.ref.periph.OTG_FS.host`. Read-only."""

    __slots__ = ("_host", "_label", "_owner")

    def __init__(self, owner: object, label: str, host: UsbHost) -> None:
        """Wrap a host. Use `m.ref.periph.<NAME>.host` instead."""
        self._owner = owner
        self._label = label
        self._host = host

    @property
    def state(self) -> Ref[UsbState]:
        """Device state as the host sees it."""
        from popcorn._usb import UsbState

        host = self._host
        return Ref(
            self._owner,
            f"{self._label}.state",
            lambda: host.state,
            read=host._link.op_state,  # pyright: ignore[reportPrivateUsage]
            decode=lambda raw: UsbState(cast(int, raw)),
        )

    @property
    def address(self) -> Ref[int]:
        """Address the host gave the device."""
        host = self._host
        return Ref(
            self._owner,
            f"{self._label}.address",
            lambda: host.address,
            read=host._link.op_address,  # pyright: ignore[reportPrivateUsage]
        )

    @property
    def configuration(self) -> Ref[int]:
        """bConfigurationValue the host set."""
        host = self._host
        return Ref(
            self._owner,
            f"{self._label}.configuration",
            lambda: host.configuration,
            read=host._link.op_configuration_value,  # pyright: ignore[reportPrivateUsage]
        )

    @property
    def requests(self) -> Ref[tuple[UsbRequest, ...]]:
        """Every control request the host sent since the machine was built."""
        from popcorn._usb import requests_of

        host = self._host
        return Ref(
            self._owner,
            f"{self._label}.requests",
            lambda: host.requests,
            read=host._link.op_requests,  # pyright: ignore[reportPrivateUsage]
            decode=lambda raw: requests_of(cast(list[_core.UsbRequestRecord], raw)),
        )


class PeripheralRef:
    """References into one peripheral, `m.ref.periph.USART1`."""

    __slots__ = ("_owner", "_view")

    def __init__(self, owner: object, view: PeripheralView) -> None:
        """Wrap a peripheral view. Use `m.ref.periph.<P>` instead."""
        self._owner = owner
        self._view = view

    @property
    def _name(self) -> str:
        return self._view._name  # pyright: ignore[reportPrivateUsage]

    def __getattr__(self, name: str) -> RegisterRef:
        return RegisterRef(self._owner, f"periph.{self._name}.{name}", getattr(self._view, name))

    def __getitem__(self, name: str) -> RegisterRef:
        return self.__getattr__(name)

    def pin(self, number: int) -> PinRefs:
        """Pin `number` of this GPIO port.

        Raises:
            ConfigError: `number` is not 0 to 15.
        """
        return PinRefs(self._owner, f"periph.{self._name}.pin({number})", self._view.pin(number))

    def input(self, channel: int) -> AnalogRefs:
        """External channel `channel` of this ADC.

        Raises:
            ConfigError: `channel` is not 1 to 17.
        """
        return AnalogRefs(
            self._owner, f"periph.{self._name}.input({channel})", self._view.input(channel)
        )

    def audio(self, block: Any) -> AudioRefs:
        """Audio block `block` of this SAI."""
        return AudioRefs(
            self._owner, f"periph.{self._name}.audio({block!r})", self._view.audio(block)
        )

    @property
    def port(self) -> PortRefs:
        """The host end of this UART."""
        return PortRefs(self._owner, f"periph.{self._name}.port", self._view.port)

    @property
    def host(self) -> HostRefs:
        """The virtual USB host of this device controller."""
        return HostRefs(self._owner, f"periph.{self._name}.host", self._view.host)


class PeripheralRefs:
    """References into the peripherals, `m.ref.periph`."""

    __slots__ = ("_owner", "_peripherals")

    def __init__(self, owner: object, peripherals: Peripherals) -> None:
        """Wrap the peripherals. Use `m.ref.periph` instead."""
        self._owner = owner
        self._peripherals = peripherals

    def __getattr__(self, name: str) -> PeripheralRef:
        return PeripheralRef(self._owner, getattr(self._peripherals, name))

    def __getitem__(self, name: str) -> PeripheralRef:
        return self.__getattr__(name)


class VarRef[T: (int, float, bool)](MemoryRef[Any]):
    """A variable, `m.ref.var("counter")`: its value as `Var.value`, a member by name and an
    element by index. A member is looked up in DWARF only, `member(name)` reaches one whose name
    `VarRef` itself uses, such as `enum_name`."""

    __slots__ = ("_var",)

    def __init__(self, owner: object, var: Var) -> None:
        """Wrap a variable view. Use `m.ref.var(name)` instead."""

        def put(value: int | float | bool) -> None:
            var.value = value

        def check(value: object) -> None:
            if not isinstance(value, int | float):
                raise TypeError(f"{var.name} takes a number, not {type(value).__name__}")

        def locate() -> ScalarLayout:
            from popcorn._dwarf_views import scalar_layout

            return scalar_layout(var)

        handle = _handle(owner)

        def read() -> _core.AccessOp:
            size = var.span()
            return handle.op_read_memory(var.address, size)

        def write(value: int | float | bool) -> _core.AccessOp:
            data, mask = var.encode(value)
            if mask is None:
                return handle.op_write_memory(var.address, data)
            return handle.op_memory_masked(var.address, mask, data)

        super().__init__(
            owner,
            f"var({var.name})",
            lambda: var.value,
            put,
            check,
            locate,
            read=read,
            decode=lambda raw: var.decode(cast(bytes, raw)),
            write=write,
        )
        self._var = var

    def member(self, name: str) -> VarRef[Any]:
        """The member `name` of this struct or union, also when `VarRef` or `Ref` has an
        attribute of that name, such as `value`, `label` or `get`. A member named `member` is
        reached this way only, `.member("member")`: `ref.member` is this method. Resolved from
        DWARF now, read at the batch.

        Raises:
            AttributeError: Not a struct or union, or it has no such member.
        """
        return VarRef(self.owner, self._var.member(name))

    def __getattr__(self, name: str) -> VarRef[Any]:
        if name.startswith("_"):
            raise AttributeError(name)
        return self.member(name)

    @property
    def enum_name(self) -> Ref[str | None]:
        """Name of the enumerator this enum holds, None when no enumerator has its value, as
        `Var.enum_name`. Read-only.

        Raises:
            TypeError: Not an enum, at the call.
        """
        from popcorn.image.dwarf import EnumType

        var = self._var
        resolved = var.type
        if not isinstance(resolved, EnumType):
            raise TypeError(f"{var.name} is not an enum")
        names: dict[int | float | bool, str] = {}
        for name, value in resolved.values.items():
            names.setdefault(value, name)
        handle = _handle(self.owner)
        return Ref(
            self.owner,
            f"{self.label}.enum_name",
            lambda: var.enum_name,
            read=lambda: handle.op_read_memory(var.address, var.span()),
            decode=lambda raw: names.get(var.decode(cast(bytes, raw))),
        )

    def __getitem__(self, index: int) -> VarRef[Any]:
        return VarRef(self.owner, self._var[index])


class Refs:
    """The reference tree of a machine, `m.ref`, parallel to `m.mem`, `m.reg` and `m.periph`."""

    __slots__ = ("_instructions", "_owner", "_pc", "_pending", "_variable", "mem", "periph", "reg")

    def __init__(
        self,
        owner: object,
        memory: Memory,
        registers: RegisterBank[Any],
        peripherals: Peripherals,
        scalars: Callable[[str], Callable[[], int]],
        variable: Callable[[str], Var],
    ) -> None:
        """Build the tree. `Machine.ref` does."""
        self._owner = owner
        self.mem = MemoryRefs(owner, memory)
        self.reg = RegisterRefs(owner, registers)
        self.periph = PeripheralRefs(owner, peripherals)
        handle = _handle(owner)
        self._pc = Ref(owner, "pc", scalars("pc"), read=handle.op_pc)
        self._instructions = Ref(
            owner, "instructions", scalars("instructions"), read=handle.op_instructions
        )
        self._pending = Ref(
            owner, "pending_events", scalars("pending_events"), read=handle.op_pending_events
        )
        self._variable = variable

    @property
    def pc(self) -> Ref[int]:
        """Address of the next instruction. Read-only."""
        return self._pc

    @property
    def instructions(self) -> Ref[int]:
        """Instructions executed since reset. Read-only."""
        return self._instructions

    @property
    def pending_events(self) -> Ref[int]:
        """Events waiting in the event queue. Read-only."""
        return self._pending

    def var(self, name: str) -> VarRef[Any]:
        """A module-scope variable, typed from DWARF.

        Raises:
            SymbolError: No loaded image declares the variable.
        """
        return VarRef(self._owner, self._variable(name))

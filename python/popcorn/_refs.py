"""References to machine state for `Machine.read` and `Machine.write`, and their results."""

from __future__ import annotations

import enum
import struct
from collections.abc import Callable, Coroutine, Generator
from types import TracebackType
from typing import TYPE_CHECKING, Any, override

from popcorn import _core
from popcorn._enums import PinMode
from popcorn._host import awaited
from popcorn.units import VirtualTime

if TYPE_CHECKING:
    from popcorn._dwarf_views import Var
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


class Pending[R](Coroutine[Any, Any, R]):
    """A command posted to a machine. Await it for its result.

    The command is posted when the method returning it is called, not when it is awaited: a
    stamped command waits for its time even while nothing awaits it. A coroutine, so
    `asyncio.run` and `asyncio.create_task` take it. Cancelling the awaiting task withdraws a
    command not yet run. A command whose exception is never retrieved reports it to
    `sys.unraisablehook` when the Pending is collected.
    """

    __slots__ = ("_cancel", "_convert", "_finished", "_follow", "_future", "_inner", "_spent")

    def __init__(
        self,
        future: _core.HostFuture,
        convert: Callable[[object], Any],
        *,
        cancel: bool = True,
        follow: Callable[[Any], Coroutine[Any, Any, R]] | None = None,
    ) -> None:
        """Wrap a posted command. `follow` continues from its result when first awaited,
        `cancel` False leaves the command in place when the awaiting task is cancelled."""
        self._future: _core.HostFuture | None = future
        self._convert = convert
        self._cancel = cancel
        self._follow = follow
        self._inner: Coroutine[Any, Any, R] | None = None
        self._spent = False
        self._finished = False

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
        error = typ if isinstance(typ, BaseException) else typ()
        return self._coroutine().throw(error)

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
        try:
            value = await awaited(future, self._convert, cancel=self._cancel)
        finally:
            self._future = None
            self._finished = True
            del future
        if self._follow is not None:
            return await self._follow(value)
        result: R = value
        return result

    def __del__(self) -> None:
        future = self._future
        if not self._spent and self._inner is None and future is not None and future.done():
            self._future = None
            future.result(0.0)


class Ref[T]:
    """A location of one machine's state, `m.ref.reg.r0`. It never touches the machine:
    `Machine.read` and `Machine.write` access it, as one command."""

    __slots__ = ("_check", "_get", "_set", "label", "owner")

    def __init__(
        self,
        owner: object,
        label: str,
        get: Callable[[], T],
        put: Callable[[T], None] | None = None,
        check: Callable[[object], None] | None = None,
    ) -> None:
        """A reference. Use the tree of `Machine.ref` instead."""
        self.owner = owner
        self.label = label
        self._get = get
        self._set = put
        self._check = check

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
        """Read the location now, as the sync view does. `Machine.read` calls it on the
        machine's thread."""
        return self._get()

    def put(self, value: T) -> None:
        """Write the location now, as the sync view does. `Machine.write` calls it."""
        if self._set is None:
            raise TypeError(f"{self.label} is read-only")
        self._set(value)

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


def _packs(fmt: str) -> Callable[[object], None]:
    def check(value: object) -> None:
        try:
            struct.pack(fmt, value)
        except struct.error as error:
            raise ValueError(f"{value!r} does not fit {fmt[1:]}: {error}") from error

    return check


class ScalarRefs[T: (int, float)]:
    """References to little-endian scalars of one type, `m.ref.mem.u32[addr]`."""

    __slots__ = ("_fmt", "_owner", "_view")

    def __init__(self, owner: object, view: ScalarView[T], fmt: str) -> None:
        """Wrap a scalar view. Use `m.ref.mem.<fmt>` instead."""
        self._owner = owner
        self._view = view
        self._fmt = fmt

    def __getitem__(self, address: int) -> Ref[T]:
        view = self._view

        def put(value: T) -> None:
            view[address] = value

        return Ref(
            self._owner,
            f"mem.{self._fmt}[{address:#010x}]",
            lambda: view[address],
            put,
            _packs("<" + _FORMATS[self._fmt]),
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

        return Ref(
            self._owner,
            f"mem.block({address:#010x}, {size})",
            lambda: memory.read(address, size),
            lambda data: memory.write(address, bytes(data)),
            check,
        )

    def cstring(self, address: int, limit: int = 256) -> Ref[str]:
        """The NUL-terminated string at `address`, `Memory.cstring`. Read-only."""
        memory = self._memory
        return Ref(
            self._owner,
            f"mem.cstring({address:#010x})",
            lambda: memory.cstring(address, limit),
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

        return Ref(self._owner, f"reg.{name}", get, put, _integer(64, "register"))

    def __getitem__(self, register: enum.Enum) -> Ref[int]:
        bank = self._bank
        bank._slot(register)  # pyright: ignore[reportPrivateUsage]

        def put(value: int) -> None:
            bank[register] = value

        return Ref(
            self._owner,
            f"reg[{register.value}]",
            lambda: bank[register],
            put,
            _integer(64, "register"),
        )


class RegisterRef(Ref[int]):
    """A peripheral register, `m.ref.periph.USART1.CR1`: its stored value, as
    `RegisterView.value`. A field by name, `.UE`, and `.bus` for a bus access a model reacts
    to."""

    __slots__ = ("_view",)

    def __init__(self, owner: object, label: str, view: RegisterView) -> None:
        """Wrap a register view. Use `m.ref.periph.<P>.<R>` instead."""
        width = view._width  # pyright: ignore[reportPrivateUsage]

        def put(value: int) -> None:
            view.value = value

        super().__init__(owner, label, lambda: view.value, put, _integer(width, "register"))
        self._view = view

    @property
    def bus(self) -> Ref[int]:
        """The register over the bus, `RegisterView.read` and `write`."""
        view = self._view
        width = view._width  # pyright: ignore[reportPrivateUsage]
        return Ref(
            self.owner, f"{self.label}.bus", view.read, view.write, _integer(width, "register")
        )

    def __getattr__(self, name: str) -> Ref[int]:
        view = self._view
        field = view._field(name)  # pyright: ignore[reportPrivateUsage]

        def put(value: int) -> None:
            setattr(view, name, value)

        def get() -> int:
            value: int = getattr(view, name)
            return value

        return Ref(self.owner, f"{self.label}.{name}", get, put, _integer(field.width, "field"))


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

        def put(value: bool | None) -> None:
            if value is None:
                pin.release()
            else:
                pin.level = value

        return Ref[bool](self._owner, f"{self._label}.level", lambda: pin.level, put)

    @property
    def mode(self) -> Ref[PinMode]:
        """Mode the firmware configured. Read-only."""
        pin = self._pin
        return Ref(self._owner, f"{self._label}.mode", lambda: pin.mode)

    @property
    def alternate(self) -> Ref[int]:
        """Alternate function number. Read-only."""
        pin = self._pin
        return Ref(self._owner, f"{self._label}.alternate", lambda: pin.alternate)


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

        return Ref(self._owner, f"{self._label}.volts", lambda: analog.volts, put, check)


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

        return Ref(
            self._owner, f"{self._label}.external_frame_hz", get, put, _integer(32, "frame rate")
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
        return Ref(self._owner, f"{self._label}.queued", lambda: stream.queued)


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
        host = self._host
        return Ref(self._owner, f"{self._label}.state", lambda: host.state)

    @property
    def address(self) -> Ref[int]:
        """Address the host gave the device."""
        host = self._host
        return Ref(self._owner, f"{self._label}.address", lambda: host.address)

    @property
    def configuration(self) -> Ref[int]:
        """bConfigurationValue the host set."""
        host = self._host
        return Ref(self._owner, f"{self._label}.configuration", lambda: host.configuration)

    @property
    def requests(self) -> Ref[tuple[UsbRequest, ...]]:
        """Every control request the host sent since the machine was built."""
        host = self._host
        return Ref(self._owner, f"{self._label}.requests", lambda: host.requests)


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


class VarRef[T: (int, float, bool)](Ref[Any]):
    """A variable, `m.ref.var("counter")`: its value as `Var.value`, a member by name and an
    element by index."""

    __slots__ = ("_var",)

    def __init__(self, owner: object, var: Var) -> None:
        """Wrap a variable view. Use `m.ref.var(name)` instead."""

        def put(value: int | float | bool) -> None:
            var.value = value

        def check(value: object) -> None:
            if not isinstance(value, int | float):
                raise TypeError(f"{var.name} takes a number, not {type(value).__name__}")

        super().__init__(owner, f"var({var.name})", lambda: var.value, put, check)
        self._var = var

    def __getattr__(self, name: str) -> VarRef[Any]:
        return VarRef(self.owner, getattr(self._var, name))

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
        self._pc = Ref(owner, "pc", scalars("pc"))
        self._instructions = Ref(owner, "instructions", scalars("instructions"))
        self._pending = Ref(owner, "pending_events", scalars("pending_events"))
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

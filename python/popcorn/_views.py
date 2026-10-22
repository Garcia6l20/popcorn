"""Views of the machine state: memory, core registers, peripheral registers."""

from __future__ import annotations

import enum
import math
import struct
from collections.abc import Callable, Iterable, Iterator, Sequence
from typing import TYPE_CHECKING, Any, Literal, cast

from popcorn import _core
from popcorn._enums import PinMode
from popcorn._errors import ConfigError
from popcorn._rng import MAX_QUEUED, checked_int
from popcorn.units import VirtualTime

if TYPE_CHECKING:
    from popcorn._audio import AudioStream
    from popcorn._part_description import PartDescription
    from popcorn._pdm import FilterTap, PdmMic
    from popcorn._refs import Pending
    from popcorn._streams import UartStream
    from popcorn._usb import UsbHost

type Poster = Callable[[_core.AccessOp], Pending[tuple[Any, VirtualTime]]]
"""Posts one native command to the machine, `Machine._post`: its value and its time."""

MAX_PIN_EDGES = 65536
"""Most edges one `Pin.drive_edges` call takes, and most edges a GPIO port keeps pending."""

_MAX_PS = (1 << 64) - 1


class ViewAccess:
    """What a view needs from its machine: the native handle, which refuses an access while the
    machine is busy outside its callbacks, its UART and audio streams, its USB hosts and its PDM
    microphones and filter outputs."""

    __slots__ = ("audio", "handle", "mic", "output", "ports", "post", "usb")

    def __init__(
        self,
        handle: _core.MachineHandle,
        ports: Callable[[str], UartStream] | None = None,
        audio: Callable[[str, str], AudioStream] | None = None,
        usb: Callable[[str], UsbHost] | None = None,
        post: Poster | None = None,
        mic: Callable[[str, int, str], PdmMic] | None = None,
        output: Callable[[str, int], FilterTap] | None = None,
    ) -> None:
        self.handle = handle
        self.ports = ports
        self.audio = audio
        self.usb = usb
        self.post = post
        self.mic = mic
        self.output = output


class ScalarView[T: (int, float)]:
    """Little-endian scalars of one type, indexed by guest address: `m.mem.u32[addr] = 5`."""

    __slots__ = ("_access", "_format", "_size", "_space")

    def __init__(
        self,
        access: ViewAccess,
        fmt: str,
        space: _core.MemorySpace = _core.MemorySpace.debugger,
    ) -> None:
        self._access = access
        self._format = "<" + fmt
        self._size = struct.calcsize(self._format)
        self._space = space

    def __getitem__(self, address: int) -> T:
        value: T = struct.unpack(
            self._format, self._access.handle.read_memory(address, self._size, self._space)
        )[0]
        return value

    def __setitem__(self, address: int, value: T) -> None:
        try:
            data = struct.pack(self._format, value)
        except (struct.error, OverflowError) as error:
            raise ValueError(f"{value!r} does not fit {self._format[1:]}: {error}") from error
        self._access.handle.write_memory(address, data, self._space)


def space_of[T: (int, float)](view: ScalarView[T] | Memory) -> _core.MemorySpace:
    """The view an accessor reads through: the unfiltered debugger view or a filtered one."""
    return view._space  # pyright: ignore[reportPrivateUsage]


class Memory:
    """Guest memory as the debugger sees it: flash writable, MMIO through the models.

    `secure` and `nonsecure` are the same memory seen through the core's security
    attribution and the bus firewall, as a privileged debug access of that state. A bus error
    raises BusAccessError, an EmulationError, naming the failing address and whether the bus
    denied the access or nothing is mapped there.
    """

    __slots__ = (
        "_access",
        "_filtered",
        "_space",
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

    def __init__(
        self,
        access: ViewAccess,
        space: _core.MemorySpace = _core.MemorySpace.debugger,
    ) -> None:
        self._access = access
        self._space = space
        self._filtered: tuple[Memory, Memory] | None = None
        self.u8 = ScalarView[int](access, "B", space)
        self.u16 = ScalarView[int](access, "H", space)
        self.u32 = ScalarView[int](access, "I", space)
        self.u64 = ScalarView[int](access, "Q", space)
        self.i8 = ScalarView[int](access, "b", space)
        self.i16 = ScalarView[int](access, "h", space)
        self.i32 = ScalarView[int](access, "i", space)
        self.i64 = ScalarView[int](access, "q", space)
        self.f32 = ScalarView[float](access, "f", space)
        self.f64 = ScalarView[float](access, "d", space)

    def _views(self) -> tuple[Memory, Memory]:
        if self._space != _core.MemorySpace.debugger:
            raise AttributeError("a filtered memory view has no secure or nonsecure view")
        if self._filtered is None:
            self._filtered = (
                Memory(self._access, _core.MemorySpace.secure),
                Memory(self._access, _core.MemorySpace.nonsecure),
            )
        return self._filtered

    @property
    def secure(self) -> Memory:
        """Memory as a privileged Secure debug access sees it, DDI0553B.z B13.3.4.

        Each access is attributed by the SAU and the IDAU first: at a Non-secure address it
        goes out as a Non-secure access. The bus firewall then decides it as a CPU access with
        those attributes, the GTZC TZSC, MPCBB and MPCWM, the FLASH SECBB, PRIVBB and HDP, and
        the TrustZone-aware peripherals: a refused read reads 0, a refused write is dropped. A
        write to flash follows the rule of a read there. No illegal access event is raised,
        the TZIC flags and GTZC_IRQn stay as they are. Unmodelled data raises ModelError as
        through `m.mem`. On a machine without TrustZone enabled it acts as `m.mem`.
        """
        return self._views()[0]

    @property
    def nonsecure(self) -> Memory:
        """Memory as a privileged Non-secure debug access sees it, DDI0553B.z B13.3.4.

        An access to an address the SAU or the IDAU makes Secure or Non-secure callable raises
        BusAccessError with `denied` True, "bus error at <address>: denied", the debug access
        error of RWMRR.
        Every other access goes through the bus firewall as `secure` describes, so Secure
        data never reads through this view. On a machine without TrustZone enabled it acts as
        `m.mem`.
        """
        return self._views()[1]

    def read(self, address: int, size: int) -> bytes:
        """`size` bytes from `address`."""
        return self._access.handle.read_memory(address, size, self._space)

    def write(self, address: int, data: bytes) -> None:
        """Write `data` at `address`.

        Through `secure` or `nonsecure` the bytes go out one access at a time, as a debug
        access port does: when an access raises BusAccessError, the bytes before it are
        written and the rest are not.
        """
        self._access.handle.write_memory(address, data, self._space)

    def write_masked(self, address: int, mask: bytes, data: bytes) -> None:
        """Write the bits of `data` that `mask` selects at `address`, in one access."""
        self._access.handle.write_memory_masked(address, mask, data, self._space)

    def cstring(self, address: int, limit: int = 256) -> str:
        """The NUL-terminated string at `address`, at most `limit` bytes, UTF-8 with
        replacement characters.

        Reads up to the end of each 64-byte block at once, byte by byte where a block read
        meets a bus error, so a string ending just before unmapped memory still reads.
        """
        return self._access.handle.cstring(address, limit, self._space).decode(
            "utf-8", errors="replace"
        )


class RegisterBank[R: enum.Enum]:
    """Core registers by typed enum, `bank[Reg.PSP]`. The generated `Registers` adds one typed
    attribute per register."""

    __slots__ = ("_access", "_index")

    def __init__(self, access: ViewAccess, names: Sequence[str]) -> None:
        self._access = access
        self._index = {name: index for index, name in enumerate(names)}

    def _slot(self, register: R) -> int:
        name = str(register.value)
        index = self._index.get(name)
        if index is None:
            raise ConfigError(
                f"this core has no register {name}",
                file="registers",
                key=name,
                expected="a register of this core",
            )
        return index

    def __getitem__(self, register: R) -> int:
        return self._access.handle.read_register(self._slot(register))

    def __setitem__(self, register: R, value: int) -> None:
        if value < 0 or value >> 64:
            raise ValueError(f"{value} does not fit a register")
        self._access.handle.write_register(self._slot(register), value)

    def names(self) -> tuple[str, ...]:
        """Register names of this core, in the arch module's order."""
        return tuple(self._index)


class FieldBits:
    """Position of a register field."""

    __slots__ = ("lsb", "mask", "width")

    def __init__(self, lsb: int, width: int) -> None:
        self.lsb = lsb
        self.width = width
        self.mask = ((1 << width) - 1) << lsb


class RegisterView:
    """One peripheral register: `.value` for the whole word, a field by name as a bit slice.

    Writes go over the bus as the debugger, as `m.mem` and `write()` do, so the model and the
    description's write effects act: `.value = v` writes the word, `.FIELD = v` writes the word
    with the field's bits and every sibling field at the value its write effect ignores. Reads
    are storage peeks, so `.value` and a field getter never fire a read effect such as
    clear-on-read: `read()` is the bus read. `poke()` stores without any effect. On a
    peripheral without a model, `read()` and every write raise ModelError, as `m.mem` does,
    while `.value`, the field getters and `poke()` still reach the storage. Every access
    raises MachineRunning while the machine is busy, outside its callbacks: `m.ref` refs reach
    the register then.
    """

    __slots__ = ("_access", "_base", "_fields", "_name", "_offset", "_peripheral", "_width")

    _access: ViewAccess
    _base: int
    _fields: dict[str, FieldBits]
    _name: str
    _offset: int
    _peripheral: str
    _width: int

    def __init__(
        self, access: ViewAccess, peripheral: str, base: int, register: _core.RegisterDesc
    ) -> None:
        object.__setattr__(self, "_access", access)
        object.__setattr__(self, "_base", base)
        object.__setattr__(self, "_peripheral", peripheral)
        object.__setattr__(self, "_name", register.name)
        object.__setattr__(self, "_offset", register.offset)
        object.__setattr__(self, "_width", register.width)
        object.__setattr__(
            self, "_fields", {f.name: FieldBits(f.lsb, f.width) for f in register.fields}
        )

    @property
    def name(self) -> str:
        """Register name."""
        return self._name

    @property
    def offset(self) -> int:
        """Offset in the peripheral."""
        return self._offset

    @property
    def fields(self) -> tuple[str, ...]:
        """Field names."""
        return tuple(self._fields)

    def read(self) -> int:
        """Read the register over the bus as the debugger, so a model reacts."""
        return self._bus().__getitem__(self._base + self._offset)

    def write(self, value: int) -> None:
        """Write the register over the bus as the debugger, so a model reacts."""
        self._bus()[self._base + self._offset] = value

    def _bus(self) -> ScalarView[int]:
        memory = Memory(self._access)
        return {8: memory.u8, 16: memory.u16, 32: memory.u32}[self._width]

    @property
    def value(self) -> int:
        """Stored value of the whole register, the word as the models last updated it.

        A register a model computes lazily, such as TIM CNT or the RTC calendar TR, DR and SSR,
        shows its value at the model's last access or event, on a stopped machine as during a
        run.
        """
        return self._access.handle.peek(self._peripheral, self._offset, self._width)

    @value.setter
    def value(self, value: int) -> None:
        """Write the whole register over the bus as the debugger, as `write()` does.

        Raises:
            ValueError: `value` does not fit the register.
        """
        if value < 0 or value >> self._width:
            raise ValueError(f"{value} does not fit the {self._width}-bit register {self._name}")
        self.write(value)

    def poke(self, value: int | None = None, /, **fields: int) -> None:
        """Store the register, or some of its fields, without the model or any write effect.

        `poke(v)` stores the whole word, `poke(F=v, G=w)` stores the named fields in one store
        and keeps the other bits.

        Raises:
            TypeError: Both a value and fields, or neither, are given.
            AttributeError: A field name is not a field of the register.
            ValueError: A value does not fit the register or its field.
        """
        if (value is None) == (not fields):
            raise TypeError("poke() takes exactly one of a register value or field keywords")
        if value is not None:
            if value < 0 or value >> self._width:
                raise ValueError(
                    f"{value} does not fit the {self._width}-bit register {self._name}"
                )
            self._access.handle.poke(self._peripheral, self._offset, self._width, value)
            return
        mask = 0
        bits = 0
        for name, field_value in fields.items():
            field = self._field(name)
            if field_value < 0 or field_value >> field.width:
                raise ValueError(f"{field_value} does not fit the {field.width}-bit field {name}")
            mask |= field.mask
            bits |= field_value << field.lsb
        self._access.handle.poke_masked(self._peripheral, self._offset, self._width, mask, bits)

    def _field(self, name: str) -> FieldBits:
        field = self._fields.get(name)
        if field is None:
            raise AttributeError(
                f"{self._peripheral}.{self._name} has no field {name},"
                f" fields: {', '.join(self._fields)}"
            )
        return field

    def __getattr__(self, name: str) -> int:
        field = self._field(name)
        return (self.value & field.mask) >> field.lsb

    def __setattr__(self, name: str, value: int) -> None:
        if name == "value":
            object.__setattr__(self, name, value)
            return
        field = self._field(name)
        if value < 0 or value >> field.width:
            raise ValueError(f"{value} does not fit the {field.width}-bit field {name}")
        self._access.handle.write_field(
            self._peripheral, self._offset, self._width, field.mask, value << field.lsb
        )

    def __repr__(self) -> str:
        return f"<{self._peripheral}.{self._name} @ {self._offset:#x}>"


class PeripheralView:
    """One peripheral: its registers by name, or by alias, such as the SVD name `TIM1_CR1` the
    register `CR1` of TIM1 answers to."""

    __slots__ = ("_access", "_base", "_name", "_names", "_registers")

    def __init__(self, access: ViewAccess, peripheral: _core.PeripheralDesc) -> None:
        self._access = access
        self._name = peripheral.name
        self._base = peripheral.base
        self._names = tuple(r.name for r in peripheral.registers)
        aliases = {alias: r for r in peripheral.registers for alias in r.aliases or ()}
        self._registers = aliases | {r.name: r for r in peripheral.registers}

    @property
    def registers(self) -> tuple[str, ...]:
        """Register names. Aliases, such as the SVD name `TIM1_CR1` of `CR1`, are left out,
        and look the register up as its name does."""
        return self._names

    @property
    def port(self) -> UartStream:
        """The host end of this UART, connected on first use, replacing its board endpoint.

        Raises:
            ModelError: The peripheral's model is not a UART.
        """
        if self._access.ports is None:
            raise AttributeError(f"{self._name} has no port in this machine")
        return self._access.ports(self._name)

    def audio(self, block: Literal["A", "B"]) -> AudioStream:
        """The far end of audio block `block` of this SAI, connected at machine build.

        Raises:
            AttributeError: The machine has no audio streams.
            ModelError: The peripheral's model is not the built-in SAI.
            ConfigError: `block` is neither "A" nor "B".
        """
        if self._access.audio is None:
            raise AttributeError(f"{self._name} has no audio blocks in this machine")
        return self._access.audio(self._name, block)

    def mic(self, sitf: int, edge: Literal["rising", "falling"]) -> PdmMic:
        """The sensor on clock edge `edge` of serial interface `sitf` of this MDF or ADF,
        connected at machine build.

        Raises:
            AttributeError: The machine has no PDM microphones.
            ModelError: The peripheral's model is not the built-in MDF or ADF.
            ConfigError: `sitf` is past the last serial interface, or `edge` is neither
                "rising" nor "falling".
        """
        if self._access.mic is None:
            raise AttributeError(f"{self._name} has no microphones in this machine")
        return self._access.mic(self._name, sitf, edge)

    def output(self, filter: int) -> FilterTap:
        """The samples digital filter `filter` of this MDF or ADF writes, connected at machine
        build.

        Raises:
            AttributeError: The machine has no filter outputs.
            ModelError: The peripheral's model is not the built-in MDF or ADF.
            ConfigError: `filter` is past the last digital filter.
        """
        if self._access.output is None:
            raise AttributeError(f"{self._name} has no filter outputs in this machine")
        return self._access.output(self._name, filter)

    @property
    def host(self) -> UsbHost:
        """The virtual USB host of this device controller, built with the machine from
        `Periph(host=)` of the machine description or `Machine(usb=)`.

        Raises:
            AttributeError: The machine has no USB hosts.
            ModelError: The controller has no virtual host.
        """
        if self._access.usb is None:
            raise AttributeError(f"{self._name} has no USB host in this machine")
        return self._access.usb(self._name)

    def pin(self, number: int) -> Pin:
        """Pin `number` of this GPIO port.

        Raises:
            ConfigError: `number` is not 0 to 15.
        """
        if not 0 <= number <= 15:
            raise ConfigError(
                f"{self._name}: no pin {number}", key="pin", expected="a pin from 0 to 15"
            )
        return Pin(self._access, self._name, number)

    def input(self, channel: int) -> AnalogInput:
        """External channel `channel` of this ADC.

        Raises:
            ConfigError: `channel` is not 1 to 17.
        """
        if not 1 <= channel <= 17:
            raise ConfigError(
                f"{self._name}: no external channel {channel}",
                key="channel",
                expected="an external channel from 1 to 17",
            )
        return AnalogInput(self._access, self._name, channel)

    @property
    def entropy(self) -> RngEntropy:
        """The word source of this RNG: seed, generator position, queued and stuck words.
        Built without a native call: a peripheral whose model is not the built-in RNG raises
        ModelError at the first access of the view."""
        return RngEntropy(self._access, self._name)

    def __getattr__(self, name: str) -> RegisterView:
        register = self._registers.get(name)
        if register is None:
            raise AttributeError(f"{self._name} has no register {name}")
        return RegisterView(self._access, self._name, self._base, register)

    def __getitem__(self, name: str) -> RegisterView:
        return self.__getattr__(name)

    def __dir__(self) -> list[str]:
        return [*self._names]


class Pin:
    """One GPIO pin, `m.periph.GPIOB.pin(5)`, through the GPIO model.

    Setting `level` drives the pin from outside the MCU: an input reads it, an output keeps its
    own level. The drive is board state, kept across an MCU reset and restored by snapshots.
    While the machine is busy, every access raises MachineRunning outside its callbacks:
    `m.write((m.ref.periph.GPIOA.pin(0).level, True))` drives the pin at a block boundary then.
    `drive_at` and `drive_edges` drive it at exact virtual times instead, parked or running. A
    pin in alternate function mode gives its edges to the input its AF selects, such as
    `tim3_ch1` or `tim2_etr`. Every access raises ModelError when the port's model is not a
    GPIO port.
    """

    __slots__ = ("_access", "number", "port")

    def __init__(self, access: ViewAccess, port: str, number: int) -> None:
        """View a pin. Use `m.periph.<GPIOx>.pin(n)` instead."""
        self._access = access
        self.port = port
        self.number = number

    @property
    def level(self) -> bool:
        """Level as the input data register reads it."""
        return self._access.handle.gpio_level(self.port, self.number)

    @level.setter
    def level(self, value: bool | int) -> None:
        self._access.handle.gpio_drive(self.port, self.number, bool(value))

    def release(self) -> None:
        """Stop driving the pin, leaving it to its pull.

        Raises:
            MachineRunning: The machine is busy, outside its callbacks.
        """
        self._access.handle.gpio_drive(self.port, self.number, None)

    def drive_at(self, level: bool, *, at: VirtualTime) -> Pending[VirtualTime]:
        """Drive the pin to `level` at exactly `at`, as `drive_edges([(at, level)])` does.

        Raises:
            As `drive_edges`.
        """
        return self.drive_edges([(at, level)])

    def drive_edges(self, edges: Sequence[tuple[VirtualTime, bool]]) -> Pending[VirtualTime]:
        """Drive the pin to each level at its time, posted as one command as `UartStream.write`
        is: at once while the machine is parked, at the next block boundary while it runs.

        Each edge applies at exactly its `VirtualTime`, machine time since reset, through a GPIO
        model event, whatever the block boundary the command ran at: the level reaches the
        input data register, the pin listeners, EXTI and, in alternate function mode, the input
        the AF selects. An edge at the time the command runs applies at the next event dispatch,
        before the next instruction. Edges of one time apply in list order, after the model
        events of that instant. The list is checked whole before anything is scheduled: it
        schedules every edge or none.

        Pending edges are board state, as a drive is: snapshots save them, and an MCU reset of
        any kind keeps each at its instant, so an edge due at `m.time` T before a reset at R
        applies at `m.time` T - R after it. After a reset, edges of one instant on different
        ports apply in port order. A `level` set or a `release` applies at once and leaves
        pending edges in place. Await the result for the time the command ran at.

        Raises:
            TypeError: `edges` is no sequence of `(VirtualTime, bool)` pairs.
            ValueError: `edges` holds no edge or more than 65536, is not sorted by time, or an
                edge time is outside 0 to 2**64 - 1 ps. From the await, the first edge is
                already past when the command runs.
            ConfigError: From the await, the port would keep more than 65536 pending edges.
            ModelError: The port's model is not a GPIO port.
            RuntimeError: Called from a handler, a hook or a model.
        """
        if isinstance(edges, str | bytes | bytearray) or not isinstance(
            cast(object, edges), Sequence
        ):
            raise TypeError(f"drive_edges() takes a sequence of (VirtualTime, bool), not {edges!r}")
        if not 1 <= len(edges) <= MAX_PIN_EDGES:
            raise ValueError(f"drive_edges() takes 1 to {MAX_PIN_EDGES} edges, not {len(edges)}")
        times: list[int] = []
        levels: list[int] = []
        for edge in edges:
            if not isinstance(cast(object, edge), tuple) or len(edge) != 2:
                raise TypeError(f"a pin edge is a (VirtualTime, bool) pair, not {edge!r}")
            at, level = edge
            if not isinstance(cast(object, at), VirtualTime):
                raise TypeError(f"a pin edge time is a VirtualTime, not {at!r}")
            if not isinstance(cast(object, level), bool):
                raise TypeError(f"a pin edge level is a bool, not {level!r}")
            if not 0 <= at.ps <= _MAX_PS:
                raise ValueError(f"a pin edge at {at} leaves the 64-bit picosecond range")
            if times and at.ps < times[-1]:
                raise ValueError(f"pin edges are sorted by time, {at} follows a later edge")
            times.append(at.ps)
            levels.append(int(level))
        command = self._access.handle.command_gpio_edges(self.port, self.number, times, levels)
        post = self._access.post
        if post is None:
            raise RuntimeError(f"{self!r} belongs to no machine that posts commands")
        return post(command).mapped(lambda done: done[1])

    @property
    def mode(self) -> PinMode:
        """Mode the firmware configured."""
        return PinMode(self._access.handle.gpio_mode(self.port, self.number).value)

    @property
    def alternate(self) -> int:
        """Alternate function number."""
        return self._access.handle.gpio_alternate(self.port, self.number)

    def __repr__(self) -> str:
        return f"Pin({self.port}, {self.number})"


class AnalogInput:
    """One external ADC channel, `m.periph.ADC1.input(5)`, through the ADC model.

    Setting `volts` drives the channel from outside the MCU, in whole microvolts. The drive is
    board state, kept across an MCU reset and restored by snapshots. A conversion reads the value
    in force when it ends: V x 16384 / VDDA at 14 bits, floored, saturated at VDDA, VDDA from
    `SupplyOptions`. While the machine is busy, every access raises MachineRunning outside its
    callbacks: `m.write((m.ref.periph.ADC1.input(5).volts, 1.5))` drives it at a block boundary
    then. Every access raises ModelError when the peripheral's model is not the built-in ADC.
    """

    __slots__ = ("_access", "adc", "channel")

    def __init__(self, access: ViewAccess, adc: str, channel: int) -> None:
        """View a channel. Use `m.periph.<ADCx>.input(n)` instead."""
        self._access = access
        self.adc = adc
        self.channel = channel

    @property
    def volts(self) -> float:
        """Voltage driven on the channel, 0.0 when never driven."""
        return self._access.handle.adc_input(self.adc, self.channel) / 1e6

    @volts.setter
    def volts(self, value: float) -> None:
        self._access.handle.adc_drive(self.adc, self.channel, microvolts_of(value))

    def __repr__(self) -> str:
        return f"AnalogInput({self.adc}, {self.channel})"


class RngEntropy:
    """The word source of the built-in RNG model, `m.periph.RNG.entropy`.

    Every value is board state: kept across every reset kind and restored by snapshots. A
    conditioning round takes its four words from `stuck` while it is set, else from the queue,
    else from the generator. While the machine is busy, every access raises MachineRunning
    outside its callbacks.

    Raises:
        ModelError: On any access, when the peripheral's model is not the built-in RNG.
    """

    __slots__ = ("_access", "rng")

    def __init__(self, access: ViewAccess, rng: str) -> None:
        """View a source. Use `m.periph.RNG.entropy` instead."""
        self._access = access
        self.rng = rng

    @property
    def seed(self) -> int:
        """Seed of the generator, `RngOptions.seed`."""
        return self._access.handle.rng_seed(self.rng)

    @property
    def drawn(self) -> int:
        """Generator position: generator words produced since the machine was built, skipped
        zeros included. Queued and stuck words do not move it."""
        return self._access.handle.rng_drawn(self.rng)

    @property
    def queued(self) -> int:
        """Queued words not yet produced."""
        return self._access.handle.rng_queued(self.rng)

    def queue(self, words: Iterable[int]) -> None:
        """Append words produced before the generator, in order, 0 included.

        Raises:
            TypeError: A word is a bool or not an int.
            ValueError: A word is outside 0 to 2**32 - 1, or the queue would hold more than
                2**16 words. Nothing is queued then.
        """
        checked = [checked_int("words", word, 0xFFFF_FFFF) for word in words]
        if len(checked) > MAX_QUEUED - self.queued:
            raise ValueError(
                f"words: at most {MAX_QUEUED} queued words, not {len(checked)} more on "
                f"{self.queued}"
            )
        self._access.handle.rng_queue(self.rng, checked)

    @property
    def stuck(self) -> int | None:
        """The word every new word takes, None when none is set. Words already in the FIFO
        and the conditioning register keep their values.

        Raises:
            TypeError: Set to a bool, or to neither an int nor None.
            ValueError: Set outside 0 to 2**32 - 1.
        """
        return self._access.handle.rng_stuck(self.rng)

    @stuck.setter
    def stuck(self, word: int | None) -> None:
        checked = None if word is None else checked_int("stuck", word, 0xFFFF_FFFF)
        self._access.handle.rng_set_stuck(self.rng, checked)

    def __repr__(self) -> str:
        return f"RngEntropy({self.rng})"


def microvolts_of(volts: float) -> int:
    """`volts` in whole microvolts.

    Raises:
        ValueError: `volts` is not a voltage from 0 to 4294.967295 V.
    """
    microvolts = round(volts * 1e6) if math.isfinite(volts) else -1
    if not 0 <= microvolts <= 0xFFFF_FFFF:
        raise ValueError(f"{volts!r} V is not a voltage from 0 to 4294.967295 V")
    return microvolts


class Peripherals:
    """The peripherals of the description, by name: `m.periph.USART1.CR1.UE`."""

    __slots__ = ("_access", "_peripherals")

    def __init__(self, access: ViewAccess, description: PartDescription) -> None:
        self._access = access
        self._peripherals = {p.name: p for p in description.peripherals}

    def names(self) -> tuple[str, ...]:
        """Peripheral names, in description order."""
        return tuple(self._peripherals)

    def __getattr__(self, name: str) -> PeripheralView:
        peripheral = self._peripherals.get(name)
        if peripheral is None:
            raise AttributeError(f"the description has no peripheral {name}")
        return PeripheralView(self._access, peripheral)

    def __getitem__(self, name: str) -> PeripheralView:
        return self.__getattr__(name)

    def __iter__(self) -> Iterator[str]:
        return iter(self._peripherals)

    def __dir__(self) -> list[str]:
        return [*self._peripherals]

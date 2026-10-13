"""Views of the machine state: memory, core registers, peripheral registers."""

from __future__ import annotations

import enum
import math
import struct
from collections.abc import Callable, Iterator, Sequence
from typing import TYPE_CHECKING, Literal

from popcorn import _core
from popcorn._enums import PinMode
from popcorn._errors import ConfigError

if TYPE_CHECKING:
    from popcorn._audio import AudioStream
    from popcorn._description import PartDescription
    from popcorn._streams import UartStream
    from popcorn._usb import UsbHost


class ViewAccess:
    """What a view needs from its machine: the native handle, which refuses an access while the
    machine is busy outside its callbacks, its UART and audio streams and its USB hosts."""

    __slots__ = ("audio", "handle", "ports", "usb")

    def __init__(
        self,
        handle: _core.MachineHandle,
        ports: Callable[[str], UartStream] | None = None,
        audio: Callable[[str, str], AudioStream] | None = None,
        usb: Callable[[str], UsbHost] | None = None,
    ) -> None:
        self.handle = handle
        self.ports = ports
        self.audio = audio
        self.usb = usb


class ScalarView[T: (int, float)]:
    """Little-endian scalars of one type, indexed by guest address: `m.mem.u32[addr] = 5`."""

    __slots__ = ("_access", "_format", "_size")

    def __init__(self, access: ViewAccess, fmt: str) -> None:
        self._access = access
        self._format = "<" + fmt
        self._size = struct.calcsize(self._format)

    def __getitem__(self, address: int) -> T:
        value: T = struct.unpack(
            self._format, self._access.handle.read_memory(address, self._size)
        )[0]
        return value

    def __setitem__(self, address: int, value: T) -> None:
        try:
            data = struct.pack(self._format, value)
        except (struct.error, OverflowError) as error:
            raise ValueError(f"{value!r} does not fit {self._format[1:]}: {error}") from error
        self._access.handle.write_memory(address, data)


class Memory:
    """Guest memory as the debugger sees it: flash writable, MMIO through the models.

    A bus error raises EmulationError naming the failing address.
    """

    __slots__ = ("_access", "f32", "f64", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64")

    def __init__(self, access: ViewAccess) -> None:
        self._access = access
        self.u8 = ScalarView[int](access, "B")
        self.u16 = ScalarView[int](access, "H")
        self.u32 = ScalarView[int](access, "I")
        self.u64 = ScalarView[int](access, "Q")
        self.i8 = ScalarView[int](access, "b")
        self.i16 = ScalarView[int](access, "h")
        self.i32 = ScalarView[int](access, "i")
        self.i64 = ScalarView[int](access, "q")
        self.f32 = ScalarView[float](access, "f")
        self.f64 = ScalarView[float](access, "d")

    def read(self, address: int, size: int) -> bytes:
        """`size` bytes from `address`."""
        return self._access.handle.read_memory(address, size)

    def write(self, address: int, data: bytes) -> None:
        """Write `data` at `address`."""
        self._access.handle.write_memory(address, data)

    def write_masked(self, address: int, mask: bytes, data: bytes) -> None:
        """Write the bits of `data` that `mask` selects at `address`, in one access."""
        self._access.handle.write_memory_masked(address, mask, data)

    def cstring(self, address: int, limit: int = 256) -> str:
        """The NUL-terminated string at `address`, at most `limit` bytes, UTF-8 with
        replacement characters.

        Reads up to the end of each 64-byte block at once, byte by byte where a block read
        meets a bus error, so a string ending just before unmapped memory still reads.
        """
        return self._access.handle.cstring(address, limit).decode("utf-8", errors="replace")


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
    clear-on-read: `read()` is the bus read. `poke()` stores without any effect. Every access
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
    """One peripheral: its registers by name."""

    __slots__ = ("_access", "_base", "_name", "_registers")

    def __init__(self, access: ViewAccess, peripheral: _core.PeripheralDesc) -> None:
        self._access = access
        self._name = peripheral.name
        self._base = peripheral.base
        self._registers = {r.name: r for r in peripheral.registers}

    @property
    def registers(self) -> tuple[str, ...]:
        """Register names."""
        return tuple(self._registers)

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

    def __getattr__(self, name: str) -> RegisterView:
        register = self._registers.get(name)
        if register is None:
            raise AttributeError(f"{self._name} has no register {name}")
        return RegisterView(self._access, self._name, self._base, register)

    def __getitem__(self, name: str) -> RegisterView:
        return self.__getattr__(name)

    def __dir__(self) -> list[str]:
        return [*self._registers]


class Pin:
    """One GPIO pin, `m.periph.GPIOB.pin(5)`, through the GPIO model.

    Setting `level` drives the pin from outside the MCU: an input reads it, an output keeps its
    own level. The drive is board state, kept across an MCU reset and restored by snapshots.
    While the machine is busy, every access raises MachineRunning outside its callbacks:
    `m.write((m.ref.periph.GPIOA.pin(0).level, True))` drives the pin at a block boundary then.
    Every access raises ModelError when the port's model is not a GPIO port.
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

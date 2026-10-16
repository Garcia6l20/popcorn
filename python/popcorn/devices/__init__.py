"""Native bus devices: an AT25SF serial NOR flash on SPI, a 24xx EEPROM and a table-scripted
target on I2C.

Attach them as `popcorn.board` targets are, with `Machine(devices=)` or a machine description's
`Periph(devices=[...])`, for example
`Device(At25sf, jedec=[0x1F, 0x87, 0x01], size=4194304)`. They run in
C++, the bus models call them without Python. Their state is part of every machine snapshot,
and they reset on POWER_ON and BROWN_OUT as the native devices document.

Durations are VirtualDuration values of a whole number of nanoseconds. A machine description writes
them with `popcorn.units`, such as `us(600)`, and the register keys of `TableTarget` replies as
integers.

`contents()`, `snapshot()`, `restore()` and `TableTarget.writes()` need the machine the device
serves parked, or a call from one of its callbacks: while it is busy they raise
`MachineRunning`. A device that serves no machine has no such limit. `restore()` on an attached
device sets its state behind the bus model, which keeps its own transfer state.

`SpiTargetBase`, `I2CTargetBase` and `SpiControllerBase` are the native bases a plugin's device
classes derive from. `chip_select` gives an SPI device class a native plugin binds its chip
select.
"""

from __future__ import annotations

import os
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from operator import attrgetter

from popcorn import _core
from popcorn._devices import BusDevice, check_address, parked
from popcorn._devices import chip_select as _chip_select
from popcorn._errors import ConfigError
from popcorn.devices import _native
from popcorn.units import VirtualDuration, ms, s, us

__all__ = [
    "At25sf",
    "Eeprom24xx",
    "I2CTargetBase",
    "SpiControllerBase",
    "SpiTargetBase",
    "TableTarget",
    "TableWrite",
    "chip_select",
]

SpiTargetBase = _core.SpiTargetBase
"""The base to subclass for a native plugin's SPI device: the class a plugin binds on
`popcorn::periph::spi_target` derives from it, and a `TYPE_CHECKING` declaration of that class
names it as its base. A device written in Python subclasses `popcorn.board.SpiTarget`."""

I2CTargetBase = _core.I2CTargetBase
"""The base to subclass for a native plugin's I2C target: the class a plugin binds on
`popcorn::periph::i2c_target` derives from it, and a `TYPE_CHECKING` declaration of that class
names it as its base. A target written in Python subclasses `popcorn.board.I2CTarget`."""

SpiControllerBase = _core.SpiControllerBase
"""The base to subclass for a native plugin's SPI controller: the class a plugin binds on
`popcorn::periph::spi_controller_sink` derives from it, and a `TYPE_CHECKING` declaration of that
class names it as its base. A controller written in Python subclasses
`popcorn.board.SpiController`."""

_T_PP = us(600)
_T_SE = ms(60)
_T_BE1 = ms(200)
_T_BE2 = ms(300)
_T_CE = s(30)
_T_W = ms(5)
_T_DP = us(3)
_T_RES1 = us(3)
_T_RST = us(30)
_T_WR = ms(5)


def _uint(name: str, value: object, bits: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << bits:
        raise ValueError(f"{name}: a {bits}-bit unsigned int, not {value!r}")
    return value


def _ns(name: str, value: object) -> int:
    if not isinstance(value, VirtualDuration):
        raise TypeError(f"{name}: a VirtualDuration, not {value!r}")
    if value.ps < 0 or value.ps % 1000:
        raise ValueError(f"{name}: a whole, non-negative number of nanoseconds, not {value}")
    return value.ps // 1000


def _path(image: str | os.PathLike[str] | None) -> str:
    return "" if image is None else os.fspath(image)


def _built(build: Callable[[], None], keys: Mapping[str, str]) -> None:
    try:
        build()
    except ConfigError as error:
        raise ValueError(f"{keys.get(error.key, error.key)}: {error.expected}") from error


def _contents(device: _native.At25sfBase | _native.Eeprom24xxBase) -> bytes:
    read: Callable[[], bytes] = attrgetter("_contents")(device)
    return parked(device, read)


def _snapshot(device: BusDevice) -> bytes:
    save: Callable[[], bytes] = attrgetter("_snapshot")(device)
    return parked(device, save)


def _restore(device: BusDevice, data: bytes) -> None:
    load: Callable[[bytes], None] = attrgetter("_restore")(device)
    parked(device, lambda: load(data))


def chip_select(cs: str | None) -> tuple[str, int | None]:
    """The chip select of an SPI device whose class a native plugin binds.

    An SPI device attached to a machine carries its chip select in two attributes, which the
    machine reads when it attaches the device: `_cs_port`, the GPIO port name such as "GPIOB",
    and `_cs_pin`, the pin number. `_cs_port` "" with `_cs_pin` None selects the SPI hardware
    SS output. The machine also derives the pin name `clocked_by` matches, such as "SPI1:PB0",
    from these two attributes. `popcorn.board.SpiTarget` and `At25sf` set them. A Python
    subclass of a plugin's device base sets them from this function in its `__init__`:

    ```python
    class Adc(PLUGIN.module().AdcBase):
        def __init__(self, cs: str | None = None) -> None:
            super().__init__()
            self._cs_port, self._cs_pin = chip_select(cs)
    ```

    Args:
        cs: Chip select pin such as "PB0", active low, None for the hardware SS output.

    Returns:
        `(port, pin)`, `("GPIOB", 0)` for "PB0", `("", None)` for None.

    Raises:
        ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15.
    """
    return _chip_select(cs)


class At25sf(_native.At25sfBase):
    """An AT25SF serial NOR flash on an SPI bus, the native model.

    Standard SPI instructions: reads, page program, 4, 32 and 64 KB and chip erases, status
    registers, deep power down, the JEDEC, manufacturer and device IDs, reset. A program, erase
    or status write keeps BUSY set for its time in virtual time. The defaults describe the
    AT25SF641, 8 MiB, JEDEC ID 1F 32 17, with the typical times of its datasheet.

    `Machine.power_cycle()` while BUSY tears a program or erase: the first part of its bytes
    keeps the result in proportion to the time elapsed, the rest keeps the old bytes, in block
    order for an erase and in the order the data was sent for a program, from its start address
    and wrapping inside the page. This prefix rule is the model's deterministic choice, not
    datasheet behaviour. A program or erase keeps a copy of its range, up to the whole array for
    a chip erase, until the next selection after BUSY ends, a 66h 99h reset or a power cut. A
    snapshot taken before then carries it.
    """

    def __init__(
        self,
        cs: str | None = None,
        *,
        size: int = 8 << 20,
        jedec: Sequence[int] = (0x1F, 0x32, 0x17),
        device_id: int = 0x16,
        image: str | os.PathLike[str] | None = None,
        initial: bytes | Sequence[int] | None = None,
        page_program: VirtualDuration = _T_PP,
        erase_4k: VirtualDuration = _T_SE,
        erase_32k: VirtualDuration = _T_BE1,
        erase_64k: VirtualDuration = _T_BE2,
        chip_erase: VirtualDuration = _T_CE,
        write_status: VirtualDuration = _T_W,
        power_down: VirtualDuration = _T_DP,
        release: VirtualDuration = _T_RES1,
        reset: VirtualDuration = _T_RST,
    ) -> None:
        """Create the flash, its array erased, read from `image` or copied from `initial`.

        Args:
            cs: Chip select pin such as "PA4", active low, None for the SPI hardware SS output.
            size: Array size in bytes, a non-zero multiple of 64 KB.
            jedec: Manufacturer, memory type and capacity bytes of the 9Fh JEDEC ID.
            device_id: Device ID the 90h and ABh instructions return.
            image: File of the initial contents, at most `size` bytes, the rest erased.
            initial: Initial contents when there is no image, at most `size` bytes.
            page_program: tPP. The other durations are tSE, tBE1, tBE2, tCE, tW, tDP, tRES1
                and tRST, in the order above.

        Raises:
            ValueError: A value is out of range, `image` and `initial` are both given, the
                image cannot be read, or a duration is not a whole number of nanoseconds.
        """
        port, pin = _chip_select(cs)
        ids = tuple(jedec)
        if len(ids) != 3:
            raise ValueError(f"jedec: 3 bytes, not {ids!r}")
        manufacturer, memory_type, capacity = (_uint("jedec", value, 8) for value in ids)
        times = {
            "page_program": _ns("page_program", page_program),
            "erase_4k": _ns("erase_4k", erase_4k),
            "erase_32k": _ns("erase_32k", erase_32k),
            "erase_64k": _ns("erase_64k", erase_64k),
            "chip_erase": _ns("chip_erase", chip_erase),
            "write_status": _ns("write_status", write_status),
            "power_down": _ns("power_down", power_down),
            "release": _ns("release", release),
            "reset": _ns("reset", reset),
        }
        first = bytes(b"" if initial is None else initial)
        _built(
            lambda: super(At25sf, self).__init__(
                manufacturer,
                memory_type,
                capacity,
                _uint("device_id", device_id, 8),
                _uint("size", size, 32),
                _path(image),
                first,
                **times,
            ),
            {"part.size": "size"},
        )
        self._cs = cs
        self._cs_port = port
        self._cs_pin = pin

    @property
    def cs(self) -> str | None:
        """The chip select pin, None for the hardware SS output."""
        return self._cs

    def contents(self) -> bytes:
        """The flash array.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        return _contents(self)

    def snapshot(self) -> bytes:
        """The device state as bytes, as a machine snapshot records it.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        return _snapshot(self)

    def restore(self, data: bytes) -> None:
        """Restore bytes `snapshot()` returned.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
            ConfigError: `data` is the state of a device of another size or initial
                contents.
        """
        _restore(self, data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} on {self._cs or 'SS'}>"


class Eeprom24xx(_native.Eeprom24xxBase):
    """A 24xx serial EEPROM on an I2C bus, the native model.

    A write sends the word address, then data within its page, wrapping in the page. The STOP
    ending a write starts the write cycle: for tWR the device acknowledges nothing, the
    acknowledge polling of 24xx drivers. Reads advance the address pointer, wrapping at the end
    of the array. The defaults describe a 24xx256.
    """

    def __init__(
        self,
        address: int = 0x50,
        *,
        size: int = 32768,
        address_bytes: int = 2,
        page_size: int = 64,
        write_cycle: VirtualDuration = _T_WR,
        image: str | os.PathLike[str] | None = None,
    ) -> None:
        """Create the EEPROM, its array erased or read from `image`.

        Args:
            address: 7-bit bus address, 0x08 to 0x77.
            size: Array size in bytes, a multiple of `page_size`, at most 256 with one address
                byte and 65536 with two.
            address_bytes: Word address bytes, 1 or 2.
            page_size: Page size in bytes, a power of two.
            write_cycle: tWR, from the STOP ending a write.
            image: File of the initial contents, at most `size` bytes, the rest erased.

        Raises:
            ValueError: A value is out of range, the geometry is inconsistent, the image cannot
                be read, or `write_cycle` is not a whole number of nanoseconds.
        """
        check_address(address)
        cycle = _ns("write_cycle", write_cycle)
        _built(
            lambda: super(Eeprom24xx, self).__init__(
                address,
                _uint("size", size, 32),
                _uint("address_bytes", address_bytes, 8),
                _uint("page_size", page_size, 32),
                cycle,
                _path(image),
            ),
            {},
        )
        self._bus_address = address

    @property
    def address(self) -> int:
        """The 7-bit address the EEPROM answers."""
        return self._bus_address

    def contents(self) -> bytes:
        """The EEPROM array.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        return _contents(self)

    def snapshot(self) -> bytes:
        """The device state as bytes, as a machine snapshot records it.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        return _snapshot(self)

    def restore(self, data: bytes) -> None:
        """Restore bytes `snapshot()` returned.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
            ConfigError: `data` is the state of a device of another size or initial
                contents.
        """
        _restore(self, data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._bus_address:#04x}>"


@dataclass(frozen=True, slots=True)
class TableWrite:
    """One write transfer a `TableTarget` received.

    Attributes:
        reg: The register address the transfer started with.
        data: The data bytes after it, in bus order.
    """

    reg: int
    data: bytes


class TableTarget(_native.TableTargetBase):
    """An I2C target scripted by a table of replies, the native model.

    A write transfer starts with the register address, and each one with data bytes is
    recorded when the next START or the STOP ends it. A read returns the reply of the last
    register written, then 0xFF past its end. Writes never change the replies, and every byte
    is acknowledged.
    """

    def __init__(
        self,
        address: int,
        *,
        register_bytes: int = 1,
        replies: Mapping[int, bytes | Sequence[int]] | None = None,
        nack_unknown: bool = False,
    ) -> None:
        """Create the target.

        Args:
            address: 7-bit bus address, 0x08 to 0x77.
            register_bytes: Register address bytes a write starts with, 1 to 4, most
                significant first.
            replies: The bytes a read from each register returns.
            nack_unknown: Refuse a read from a register without a reply, its address not
                acknowledged, instead of returning 0xFF.

        Raises:
            ValueError: A value is out of range.
        """
        check_address(address)
        table = {
            _uint("replies", reg, 32): [_uint("replies", value, 8) for value in reply]
            for reg, reply in (replies or {}).items()
        }
        _built(
            lambda: super(TableTarget, self).__init__(
                address, _uint("register_bytes", register_bytes, 8), table, bool(nack_unknown)
            ),
            {},
        )
        self._bus_address = address

    @property
    def address(self) -> int:
        """The 7-bit address the target answers."""
        return self._bus_address

    def writes(self) -> tuple[TableWrite, ...]:
        """The write transfers recorded so far, oldest first.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        recorded: Callable[[], list[tuple[int, bytes]]] = attrgetter("_writes")(self)
        return tuple(TableWrite(reg, data) for reg, data in parked(self, recorded))

    def snapshot(self) -> bytes:
        """The device state as bytes, as a machine snapshot records it.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        return _snapshot(self)

    def restore(self, data: bytes) -> None:
        """Restore bytes `snapshot()` returned.

        Raises:
            MachineRunning: The machine it serves is busy, outside its callbacks.
        """
        _restore(self, data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._bus_address:#04x}>"

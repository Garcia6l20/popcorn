"""Native bus devices: an AT25SF serial NOR flash on SPI, a 24xx EEPROM and a table-scripted
target on I2C.

Attach them as `popcorn.board` targets are, with `Machine(devices=)` or a project file's
`[board.X] devices`, for example
`{ model = "popcorn.devices:At25sf", jedec = [0x1F, 0x87, 0x01], size = 4194304 }`. They run in
C++, the bus models call them without Python. Their state is part of every machine snapshot,
and they reset on POWER_ON and BROWN_OUT as the native devices document.

Durations are VirtualDuration values of a whole number of nanoseconds. A project file writes
them as strings such as "600 us", and the register keys of `TableTarget` replies as strings
such as "0x0F" or "15".

Call `contents()`, `snapshot()` and `restore()` only from the machine's thread while it is
stopped. `restore()` on an attached device sets its state behind the bus model, which keeps
its own transfer state.
"""

from __future__ import annotations

import os
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from operator import attrgetter

from popcorn import _core
from popcorn._devices import check_address, chip_select
from popcorn._errors import ConfigError
from popcorn.units import VirtualDuration, ms, s, us

__all__ = ["At25sf", "Eeprom24xx", "TableTarget", "TableWrite"]

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


def _native(build: Callable[[], None], keys: Mapping[str, str]) -> None:
    try:
        build()
    except ConfigError as error:
        raise ValueError(f"{keys.get(error.key, error.key)}: {error.expected}") from error


class At25sf(_core.At25sfBase):
    """An AT25SF serial NOR flash on an SPI bus, the native model.

    Standard SPI instructions: reads, page program, 4, 32 and 64 KB and chip erases, status
    registers, deep power down, the JEDEC, manufacturer and device IDs, reset. A program, erase
    or status write keeps BUSY set for its time in virtual time. The defaults describe the
    AT25SF641, 8 MiB, JEDEC ID 1F 32 17, with the typical times of its datasheet.
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
        port, pin = chip_select(cs)
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
        _native(
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

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} on {self._cs or 'SS'}>"


class Eeprom24xx(_core.Eeprom24xxBase):
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
        _native(
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


class TableTarget(_core.TableTargetBase):
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
        _native(
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
        """The write transfers recorded so far, oldest first."""
        recorded: Callable[[], list[tuple[int, bytes]]] = attrgetter("_writes")(self)
        return tuple(TableWrite(reg, data) for reg, data in recorded())

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._bus_address:#04x}>"

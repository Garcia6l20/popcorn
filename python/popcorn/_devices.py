"""Devices on the I2C and SPI buses of the MCU, written in Python: `popcorn.board.I2CTarget` and
`popcorn.board.SpiTarget`, served by the native bus models."""

from __future__ import annotations

import re
import typing
import weakref

from popcorn import _core
from popcorn._enums import ResetKind
from popcorn._errors import ModelError
from popcorn._state import StateReader, declared_fields, encode
from popcorn.units import VirtualTime

_CS = re.compile(r"P([A-Z])(\d{1,2})")
_STATES: weakref.WeakKeyDictionary[type, tuple[tuple[str, type], ...]] = weakref.WeakKeyDictionary()
_ATTACHED: dict[int, weakref.ReferenceType[object]] = {}


def _state_of(
    cls: type[I2CTarget | SpiTarget], base: type[I2CTarget] | type[SpiTarget]
) -> tuple[tuple[str, type], ...]:
    state = _STATES.get(cls)
    if state is None:
        state = _STATES[cls] = declared_fields(cls, base, custom=False)[0]
    return state


def _snapshot(device: I2CTarget | SpiTarget, base: type[I2CTarget] | type[SpiTarget]) -> bytes:
    out = bytearray()
    for name, kind in _state_of(type(device), base):
        value: object = getattr(device, name)
        if not isinstance(value, kind):
            raise ModelError(
                f"{device!r}: state field {name} holds a {type(value).__name__}, "
                f"not a {kind.__name__}"
            )
        encode(out, value)
    return bytes(out)


def _restore(
    device: I2CTarget | SpiTarget, base: type[I2CTarget] | type[SpiTarget], data: bytes
) -> None:
    fields = _state_of(type(device), base)
    reader = StateReader(data)
    values = [(name, reader.read(kind)) for name, kind in fields]
    if not reader.done():
        raise ModelError(
            f"{device!r}: {len(data)} bytes of state do not match {type(device).__qualname__}"
        )
    for name, value in values:
        setattr(device, name, value)


def _saved(device: object, data: object) -> bytes:
    if not isinstance(data, bytes | bytearray | memoryview):
        raise ModelError(f"{device!r}: snapshot() returned a {type(data).__name__}, not bytes")
    return bytes(typing.cast("bytes | bytearray | memoryview[int]", data))


def _flag(device: object, method: str, value: object) -> bool:
    if not isinstance(value, bool):
        raise ModelError(f"{device!r}: {method}() returned a {type(value).__name__}, not a bool")
    return value


def _word(device: object, method: str, value: object, bits: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << bits:
        raise ModelError(f"{device!r}: {method}() returned {value!r}, not a {bits}-bit int")
    return value


def check_address(address: int) -> None:
    """Refuse `address` unless it is a 7-bit I2C target address, 0x08 to 0x77.

    Raises:
        ValueError: It is not.
    """
    if isinstance(address, bool) or not 0x08 <= address <= 0x77:
        raise ValueError(f"an I2C target address is 0x08 to 0x77, not {address!r}")


class I2CTarget(_core.I2CTargetBase):
    """A target on an I2C bus, answering one 7-bit address, written in Python.

    Attach it with `Machine(devices={"I2C1": [target]})` or a project file's
    `[board.I2C1] devices`. The I2C model calls the `on_` methods at the end of each bus phase,
    one byte at a time: the controller decides how many bytes it reads, so a target never knows
    a transfer's length in advance. Of two targets answering one address, the first attached
    answers.

    Time: `at` is the virtual time since power-on, as a VirtualTime. Only POWER_ON restarts
    it: after a reset of another kind it keeps counting while the machine's own time restarts
    at 0, so the two are not comparable.

    State: the annotated fields of type int, bool, float, bytes, bytearray or str are what
    `snapshot()` saves. A target with other state overrides `snapshot()` and `restore()`.

    Reset: `reset()` does nothing by default, since a device on the board is not reset with the
    MCU. On POWER_ON and BROWN_OUT an override clears the volatile state and every time it
    keeps: after POWER_ON the times passed restart from 0.

    Lifetime: a target serves one machine at a time and must not keep a reference to it.
    """

    def __init__(self, address: int) -> None:
        """Answer `address`.

        Raises:
            ValueError: `address` is not a 7-bit target address, 0x08 to 0x77.
        """
        check_address(address)
        super().__init__()
        self._bus_address = address

    @property
    def address(self) -> int:
        """The 7-bit address the target answers."""
        return self._bus_address

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        """A START or repeated START addressed the target, `read` for a read transfer.

        Returns:
            Whether the target acknowledges. True by default.
        """
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        """The controller wrote `byte`.

        Returns:
            Whether the target acknowledges it. True by default.
        """
        return True

    def on_read(self, at: VirtualTime) -> int:
        """The byte the target sends to the controller, 0xFF by default."""
        return 0xFF

    def on_stop(self, at: VirtualTime) -> None:
        """A STOP ended the transfer the target took part in."""

    def reset(self, kind: ResetKind) -> None:
        """The machine was reset. Does nothing by default."""

    def snapshot(self) -> bytes:
        """The target state as bytes: the declared state fields, in declaration order."""
        return _snapshot(self, I2CTarget)

    def restore(self, data: bytes) -> None:
        """Set the declared state fields from bytes `snapshot()` returned."""
        _restore(self, I2CTarget, data)

    def _address(self) -> int:
        return self._bus_address

    def _start(self, read: bool, at: int) -> bool:
        return _flag(self, "on_start", self.on_start(read, VirtualTime(at)))

    def _write(self, byte: int, at: int) -> bool:
        return _flag(self, "on_write", self.on_write(byte, VirtualTime(at)))

    def _read(self, at: int) -> int:
        return _word(self, "on_read", self.on_read(VirtualTime(at)), 8)

    def _stop(self, at: int) -> None:
        self.on_stop(VirtualTime(at))

    def _reset(self, kind: _core.ResetKind) -> None:
        self.reset(ResetKind(kind.value))

    def _save(self) -> bytes:
        return _saved(self, typing.cast(object, self.snapshot()))

    def _load(self, data: bytes) -> None:
        self.restore(data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._bus_address:#04x}>"


class SpiTarget(_core.SpiTargetBase):
    """A device on an SPI bus, written in Python.

    Attach it with `Machine(devices={"SPI1": [device]})` or a project file's
    `[board.SPI1] devices`. The SPI model calls `on_select` on each chip select edge and
    `on_exchange` for each frame while the device is selected, full duplex. Devices sharing a
    chip select, or several on the hardware SS output, are all selected and see every frame,
    their replies ANDed as on an open-drain MISO.

    Chip select: `cs` names the GPIO pin, such as "PA4", active low. The SPI model follows the
    pin from the machine's first reset or snapshot load on. None selects the device by the SPI
    hardware SS output, from the start to the end of each transfer.

    Time, state, reset and lifetime: as `I2CTarget`.
    """

    def __init__(self, cs: str | None = None) -> None:
        """Select the device through GPIO pin `cs`, or the hardware SS output when None.

        Raises:
            ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15.
        """
        port = ""
        pin: int | None = None
        if cs is not None:
            matched = _CS.fullmatch(cs)
            if matched is None or int(matched[2]) > 15:
                raise ValueError(f'a chip select is a pin name such as "PA4", not {cs!r}')
            port, pin = f"GPIO{matched[1]}", int(matched[2])
        super().__init__()
        self._cs = cs
        self._cs_port = port
        self._cs_pin = pin

    @property
    def cs(self) -> str | None:
        """The chip select pin, None for the hardware SS output."""
        return self._cs

    def on_select(self, selected: bool, at: VirtualTime) -> None:
        """The chip select changed, `selected` when it went low."""

    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        """Exchange one frame of `bits` bits, 4 to 32: `out` from the controller, the frame
        returned to it. All ones by default, an idle MISO."""
        return (1 << bits) - 1

    def reset(self, kind: ResetKind) -> None:
        """The machine was reset. Does nothing by default."""

    def snapshot(self) -> bytes:
        """The device state as bytes: the declared state fields, in declaration order."""
        return _snapshot(self, SpiTarget)

    def restore(self, data: bytes) -> None:
        """Set the declared state fields from bytes `snapshot()` returned."""
        _restore(self, SpiTarget, data)

    def _select(self, selected: bool, at: int) -> None:
        self.on_select(selected, VirtualTime(at))

    def _exchange(self, out: int, bits: int, at: int) -> int:
        return _word(self, "on_exchange", self.on_exchange(out, bits, VirtualTime(at)), bits)

    def _reset(self, kind: _core.ResetKind) -> None:
        self.reset(ResetKind(kind.value))

    def _save(self) -> bytes:
        return _saved(self, typing.cast(object, self.snapshot()))

    def _load(self, data: bytes) -> None:
        self.restore(data)

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} on {self._cs or 'SS'}>"


def serving(device: I2CTarget | SpiTarget) -> object | None:
    """The live machine `device` serves, None when it serves none."""
    held = _ATTACHED.get(id(device))
    return None if held is None else held()


def attach(device: I2CTarget | SpiTarget, machine: object) -> None:
    """Mark `device` as serving `machine`. The mark is weak: once that machine is gone the
    device may serve another one. Keyed by identity, so a device defining `__eq__` works."""
    key = id(device)
    if key not in _ATTACHED:
        weakref.finalize(device, _ATTACHED.pop, key, None)
    _ATTACHED[key] = weakref.ref(machine)

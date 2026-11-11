from __future__ import annotations

import typing
import weakref
from collections.abc import Callable
from typing import cast

from popcorn import _core
from popcorn._enums import I2CError, ResetKind
from popcorn._errors import ModelError
from popcorn._state import StateReader, declared_fields, encode
from popcorn.units import VirtualDuration, VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._audio import Block
    from popcorn._devices.i2c_target import I2CTarget
    from popcorn._devices.spi_target import SpiTarget
    from popcorn._refs import Pending

_STATES: weakref.WeakKeyDictionary[type, tuple[tuple[str, type], ...]] = weakref.WeakKeyDictionary()
ATTACHED: dict[int, _Attachment] = {}


def _state_of(
    cls: type[I2CTarget | SpiTarget], base: type[I2CTarget] | type[SpiTarget]
) -> tuple[tuple[str, type], ...]:
    state = _STATES.get(cls)
    if state is None:
        state = _STATES[cls] = declared_fields(cls, base, custom=False)[0]
    return state


def snapshot_state(device: I2CTarget | SpiTarget, base: type[I2CTarget] | type[SpiTarget]) -> bytes:
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


def restore_state(
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


def saved(device: object, data: object) -> bytes:
    if not isinstance(data, bytes | bytearray | memoryview):
        raise ModelError(f"{device!r}: snapshot() returned a {type(data).__name__}, not bytes")
    return bytes(typing.cast("bytes | bytearray | memoryview[int]", data))


class _Stateful(typing.Protocol):
    def reset(self, kind: ResetKind) -> None: ...
    def snapshot(self) -> bytes: ...
    def restore(self, data: bytes) -> None: ...


def reset_trampoline(self: _Stateful, kind: _core.ResetKind) -> None:
    self.reset(ResetKind(kind.value))


def save_trampoline(self: _Stateful) -> bytes:
    return saved(self, typing.cast(object, self.snapshot()))


def load_trampoline(self: _Stateful, data: bytes) -> None:
    self.restore(data)


def word(device: object, method: str, value: object, bits: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << bits:
        raise ModelError(f"{device!r}: {method}() returned {value!r}, not a {bits}-bit int")
    return value


def reply(device: object, method: str, value: object, stretch_ps: int) -> tuple[bool, int, int]:
    if isinstance(value, I2CError):
        return True, value.value, stretch_ps
    if not isinstance(value, bool):
        raise ModelError(
            f"{device!r}: {method}() returned a {type(value).__name__}, not a bool or an I2CError"
        )
    return value, 0, stretch_ps


def read_reply(device: object, method: str, value: object, stretch_ps: int) -> tuple[int, int, int]:
    if isinstance(value, I2CError):
        return 0xFF, value.value, stretch_ps
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 0x100:
        raise ModelError(
            f"{device!r}: {method}() returned {value!r}, not an 8-bit int or an I2CError"
        )
    return value, 0, stretch_ps


type BusDevice = (
    _core.I2CTargetBase
    | _core.SpiTargetBase
    | _core.SpiControllerBase
    | _core.I2CControllerBase
    | _core.UartDeviceBase
)
"""A device the native I2C, SPI or UART models serve: a `popcorn.board` target, controller or UART
device, or a `popcorn.devices` device."""

type AudioClock = Callable[[BusDevice, str, Block, int], None]
"""What a machine does when a device sets an audio clock: the device, the SAI, the block, the
frame rate."""

type BusHold = Callable[[str, VirtualTime | None, VirtualDuration], Pending[VirtualTime]]
"""What a machine does when an I2C target holds its bus: the peripheral, the start or None for
the command's time, the length."""

type SpiTransfer = Callable[[str, VirtualTime | None, int, int, list[int]], Pending[VirtualTime]]
"""What a machine does when an SPI controller transfers: the peripheral, the start or None for
the command's time, the frame length in ps, the frame size, the frames."""

type I2CPhases = Callable[
    [str, VirtualTime | None, int, list[int], list[int]], Pending[VirtualTime]
]
"""What a machine does when an I2C controller queues phases: the peripheral, the earliest start or
None for the command's time, the phase length in ps, the phase kinds, the phase values."""

type _Attachment = tuple[
    weakref.ReferenceType[object],
    weakref.WeakMethod[AudioClock],
    weakref.WeakMethod[BusHold],
    weakref.WeakMethod[SpiTransfer],
    weakref.WeakMethod[I2CPhases],
    str,
]


def serving(device: BusDevice) -> object | None:
    """The live machine `device` serves, None when it serves none."""
    held = ATTACHED.get(id(device))
    return None if held is None else held[0]()


def parked[T](device: BusDevice, read: Callable[[], T]) -> T:
    """`read()` with the machine `device` serves parked, or from its callback.

    Raises:
        MachineRunning: That machine is busy, outside its callbacks.
    """
    handle: _core.MachineHandle | None = getattr(serving(device), "_handle", None)
    if handle is None:
        return read()
    return cast(T, handle.parked(read))

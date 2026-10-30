"""Devices on the I2C and SPI buses of the MCU, written in Python: `popcorn.board.I2CTarget`,
`popcorn.board.SpiTarget`, `popcorn.board.SpiController` and `popcorn.board.I2CController`,
served by the native bus models."""

from __future__ import annotations

import re
import typing
import weakref
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import cast

from popcorn import _core
from popcorn._enums import I2CError, I2CPhase, ResetKind
from popcorn._errors import ConfigError, ModelError
from popcorn._state import StateReader, declared_fields, encode
from popcorn.units import VirtualDuration, VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._audio import Block
    from popcorn._refs import Pending

_CS = re.compile(r"P([A-Z])(\d{1,2})")
_MAX_STRETCH_PS = 10**12
_STATES: weakref.WeakKeyDictionary[type, tuple[tuple[str, type], ...]] = weakref.WeakKeyDictionary()
_MAX_PS = (1 << 64) - 1
_MAX_SCK_HZ = 100_000_000
_MAX_FRAMES = 4096
_MAX_PHASES = 4096
_MAX_I2C_AT_PS = 1 << 63
_MIN_SCL_HZ = 1_000
_MAX_SCL_HZ = 5_000_000
_START, _WRITE, _READ, _STOP = range(4)
_ATTACHED: dict[int, _Attachment] = {}


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


def _word(device: object, method: str, value: object, bits: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 1 << bits:
        raise ModelError(f"{device!r}: {method}() returned {value!r}, not a {bits}-bit int")
    return value


def _reply(device: object, method: str, value: object, stretch_ps: int) -> tuple[bool, int, int]:
    if isinstance(value, I2CError):
        return True, value.value, stretch_ps
    if not isinstance(value, bool):
        raise ModelError(
            f"{device!r}: {method}() returned a {type(value).__name__}, not a bool or an I2CError"
        )
    return value, 0, stretch_ps


def _read_reply(
    device: object, method: str, value: object, stretch_ps: int
) -> tuple[int, int, int]:
    if isinstance(value, I2CError):
        return 0xFF, value.value, stretch_ps
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 0x100:
        raise ModelError(
            f"{device!r}: {method}() returned {value!r}, not an 8-bit int or an I2CError"
        )
    return value, 0, stretch_ps


type BusDevice = (
    _core.I2CTargetBase | _core.SpiTargetBase | _core.SpiControllerBase | _core.I2CControllerBase
)
"""A device the native I2C or SPI models serve: a `popcorn.board` target or controller, or a
`popcorn.devices` device."""

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


class DeviceBoard:
    """What a bus device may drive on the board of the machine it serves, `target.board`.

    It holds no machine: each call finds the live machine through the device's weak
    attachment, so a device keeping this object keeps nothing alive.
    """

    __slots__ = ("_device",)

    def __init__(self, device: BusDevice) -> None:
        """Act for `device`. Use `I2CTarget.board` or `SpiTarget.board` instead."""
        self._device = device

    def set_audio_clock(self, sai: str, block: Block, frame_hz: int) -> None:
        """Start, change or stop the external frame clock of an SAI block, as
        `AudioStream.external_frame_hz` does, from a bus callback at its exact virtual time.

        Raises:
            ConfigError: The device serves no live machine, or the board does not wire the
                block's clock to this device, `AudioOptions.clocked_by`.
            ValueError: `frame_hz` is outside 0 to 2**32 - 1.
        """
        held = _ATTACHED.get(id(self._device))
        clock = None if held is None else held[1]()
        if clock is None:
            raise ConfigError(
                f"{self._device!r} serves no live machine",
                key="devices",
                expected="a device serving a live machine",
            )
        clock(self._device, sai, block, frame_hz)

    def __repr__(self) -> str:
        return f"DeviceBoard({self._device!r})"


def chip_select(cs: str | None) -> tuple[str, int | None]:
    """The GPIO port and pin of chip select `cs`, ("", None) for the hardware SS output.

    Raises:
        ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15.
    """
    if cs is None:
        return "", None
    matched = _CS.fullmatch(cs)
    if matched is None or int(matched[2]) > 15:
        raise ValueError(f'a chip select is a pin name such as "PA4", not {cs!r}')
    return f"GPIO{matched[1]}", int(matched[2])


def spi_mode(owner: str, mode: int | None) -> int | None:
    """`mode` checked as an SPI mode of `owner`, CPOL << 1 | CPHA, None for no check.

    Raises:
        TypeError: `mode` is no int or None.
        ValueError: `mode` is outside 0 to 3.
    """
    if mode is None:
        return None
    if isinstance(mode, bool) or not isinstance(cast(object, mode), int):
        raise TypeError(f"{owner} takes mode as an int or None, not {mode!r}")
    if not 0 <= mode <= 3:
        raise ValueError(f"{owner} takes mode 0 to 3, not {mode}")
    return mode


def cs_of(device: _core.SpiTargetBase) -> str | None:
    """The chip select pin name of an SPI device, such as "PB0", None for the hardware SS output.

    Read from `_cs_port` and `_cs_pin`, the attributes the machine attaches the device by, and
    spelled as the SPI model names the pin.
    """
    port: object = getattr(device, "_cs_port", "")
    pin: object = getattr(device, "_cs_pin", None)
    if not isinstance(port, str) or not port or not isinstance(pin, int):
        return None
    return f"P{port[-1]}{pin}"


def check_address(address: int) -> None:
    """Refuse `address` unless it is a 7-bit I2C target address, 0x08 to 0x77.

    Raises:
        ValueError: It is not.
    """
    if isinstance(address, bool) or not 0x08 <= address <= 0x77:
        raise ValueError(f"an I2C target address is 0x08 to 0x77, not {address!r}")


class I2CTarget(_core.I2CTargetBase):
    """A target on an I2C bus, answering one 7-bit address, written in Python.

    Attach it with `Machine(devices={"I2C1": [target]})` or a machine description's
    `Periph(devices=[Device(cls, address=...)])`. The I2C model calls the `on_` methods at the end
    of each bus phase, one byte at a time: the controller decides how many bytes it reads, so a
    target never knows a transfer's length in advance. Every target on the address takes part,
    as on the open-drain bus: the address or a written byte is acknowledged when one of them
    acknowledges, each written byte reaches every target that acknowledged the address, a read
    byte is the AND of their bytes, and the longest stretch holds SCL.

    Time: `at` is the virtual time since power-on, as a VirtualTime. Only POWER_ON restarts
    it: after a reset of another kind it keeps counting while the machine's own time restarts
    at 0, so the two are not comparable.

    State: the annotated fields of type int, bool, float, bytes, bytearray or str are what
    `snapshot()` saves. A target with other state overrides `snapshot()` and `restore()`.

    Reset: `reset()` does nothing by default, since a device on the board is not reset with the
    MCU. On POWER_ON and BROWN_OUT an override clears the volatile state and every time it
    keeps: after POWER_ON the times passed restart from 0.

    Board: `board` is what the target may drive on the board, such as the SAI clock of a codec.

    Lifetime: a target serves one machine at a time and must not keep a reference to it. It may
    keep its `board`, which holds no machine.

    Errors: a hook returning an `I2CError` makes the byte it answers end with that error. The I2C
    model sets the matching ISR flag at the end of that byte and raises the error IRQ when ERRIE
    is set. Nothing is armed for a later byte. `BUS_ERROR` and `OVERRUN` leave the transfer
    running, the byte acknowledged, a read byte 0xFF. `ARBITRATION_LOST` ends the transfer for the
    controller: CR2.START cleared, no NACKF, TC or STOPF, BUSY 1 for one STOP length, then
    `on_stop`, which a PE clear inside that window delivers at the time of the write. Raising from
    a hook is not an injection: it stops the run as a handler error.
    """

    def __init__(self, address: int) -> None:
        """Answer `address`.

        Raises:
            ValueError: `address` is not a 7-bit target address, 0x08 to 0x77.
        """
        check_address(address)
        super().__init__()
        self._bus_address = address
        self._stretch_ps: int | None = None

    @property
    def address(self) -> int:
        """The 7-bit address the target answers."""
        return self._bus_address

    @property
    def board(self) -> DeviceBoard:
        """What the target may drive on the board of the machine it serves."""
        return DeviceBoard(self)

    def on_start(self, read: bool, at: VirtualTime) -> bool | I2CError:
        """A START or repeated START addressed the target, `read` for a read transfer.

        Returns:
            Whether the target acknowledges, True by default, or the `I2CError` the address
            byte ends with.

        Raises:
            ModelError: From the run, when it returns anything else.
        """
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool | I2CError:
        """The controller wrote `byte`.

        Returns:
            Whether the target acknowledges it, True by default, or the `I2CError` the byte ends
            with.

        Raises:
            ModelError: From the run, when it returns anything else.
        """
        return True

    def on_read(self, at: VirtualTime) -> int | I2CError:
        """The controller reads a byte.

        Returns:
            The byte the target sends, 0 to 0xFF, 0xFF by default, or the `I2CError` the byte
            ends with.

        Raises:
            ModelError: From the run, when it returns anything else.
        """
        return 0xFF

    def on_stop(self, at: VirtualTime) -> None:
        """A STOP ended the transfer the target took part in. The transfer runs from its START to
        its STOP across every repeated START, so a target an earlier address of the transfer
        reached hears the STOP too."""

    def stretch(self, for_: VirtualDuration) -> None:
        """Hold SCL low for `for_` after the byte the running `on_start`, `on_write` or `on_read`
        answers, delaying the next bus phase by that much. The phases keep their length. The
        last call of one hook wins, nothing carries over to the next byte. The controller's own
        stretching, waiting for TXDR or RXDR, overlaps it: the bus moves once both release SCL.
        Ignored when the hook returns `I2CError.ARBITRATION_LOST`.

        Raises:
            RuntimeError: Called outside `on_start`, `on_write` and `on_read`.
            TypeError: `for_` is no `VirtualDuration`.
            ValueError: `for_` is below 0 or above 1 s.
        """
        if not isinstance(cast(object, for_), VirtualDuration):
            raise TypeError(f"stretch() takes a VirtualDuration, not {type(for_).__name__}")
        if not 0 <= for_.ps <= _MAX_STRETCH_PS:
            raise ValueError(f"a stretch is 0 to 1 s, not {for_}")
        if self._stretch_ps is None:
            raise RuntimeError(
                f"{self!r}: stretch() is called from on_start, on_write or on_read only"
            )
        self._stretch_ps = for_.ps

    def hold_bus(
        self, *, for_: VirtualDuration, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Hold the bus of the machine the target serves as another controller would, from `at`
        for `for_`, posted as one command as `UartStream.write` is: at once while the machine is
        parked, at the next block boundary while it runs. The hold itself begins at exactly
        `at`, or at the time the command runs with `at` None, which depends on that boundary
        while running.

        BUSY reads 1 during the hold while PE is set, and a START the firmware requests waits
        for its end. A hold due while the firmware's transfer runs starts at that transfer's
        STOP, at the end of a lost arbitration, or when PE is cleared, its end unchanged: on
        silicon the firmware would lose arbitration instead, which `I2CError.ARBITRATION_LOST`
        injects. A second hold replaces one not over yet, the one
        owning the bus keeping it until its own end or the replacement's start, whichever is
        first. A reset of the machine drops it. Await the result for the hold's start.

        Raises:
            TypeError: `for_` is no `VirtualDuration`, or `at` no `VirtualTime`.
            ValueError: `for_` is not positive, `at` is negative, or the end passes 2**64 - 1
                ps. From the await, `at` is already past when the command runs.
            ConfigError: The target serves no live machine.
            RuntimeError: Called from a handler, a hook or a model.
        """
        if not isinstance(cast(object, for_), VirtualDuration):
            raise TypeError(f"hold_bus() takes for_ as a VirtualDuration, not {for_!r}")
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"hold_bus() takes at as a VirtualTime or None, not {at!r}")
        if for_.ps <= 0:
            raise ValueError(f"a bus hold lasts more than 0 ps, not {for_}")
        if for_.ps > _MAX_PS:
            raise ValueError(f"a bus hold of {for_} leaves the 64-bit picosecond range")
        if at is not None and not 0 <= at.ps <= _MAX_PS - for_.ps:
            raise ValueError(f"a bus hold from {at} for {for_} leaves the 64-bit picosecond range")
        held = _ATTACHED.get(id(self))
        hold = None if held is None else held[2]()
        if held is None or hold is None:
            raise ConfigError(
                f"{self!r} serves no live machine",
                key="devices",
                expected="a target serving a live machine",
            )
        return hold(held[5], at, for_)

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

    def _start(self, read: bool, at: int) -> tuple[bool, int, int]:
        self._stretch_ps = 0
        try:
            return _reply(self, "on_start", self.on_start(read, VirtualTime(at)), self._stretched())
        finally:
            self._stretch_ps = None

    def _write(self, byte: int, at: int) -> tuple[bool, int, int]:
        self._stretch_ps = 0
        try:
            return _reply(self, "on_write", self.on_write(byte, VirtualTime(at)), self._stretched())
        finally:
            self._stretch_ps = None

    def _read(self, at: int) -> tuple[int, int, int]:
        self._stretch_ps = 0
        try:
            return _read_reply(self, "on_read", self.on_read(VirtualTime(at)), self._stretched())
        finally:
            self._stretch_ps = None

    def _stretched(self) -> int:
        return self._stretch_ps or 0

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

    Attach it with `Machine(devices={"SPI1": [device]})` or a machine description's
    `Periph(devices=[Device(cls, cs=...)])`. The SPI model calls `on_select` on each chip select
    edge and `on_exchange` for each frame while the device is selected, full duplex. Devices
    sharing a chip select, or several on the hardware SS output, are all selected and see every
    frame, their replies ANDed as on an open-drain MISO.

    Chip select: `cs` names the GPIO pin, such as "PA4", active low. The SPI model follows the
    pin from the machine's first reset or snapshot load on. None selects the device by the SPI
    hardware SS output, from the start to the end of each transfer. The machine raises
    ConfigError at construction for a `cs` pin the part's package does not bond.

    Frame format: frames cross in wire order, the first bit on the wire in bit `bits - 1`. With
    CFG2.LSBFRST 0 that is the value the firmware writes and reads. With LSBFRST 1 the device
    sees the value bit reversed over `bits`, and its reply is reversed back before it reaches
    the RX FIFO, as on silicon: a device written for one bit order gets garbled frames from
    firmware using the other.

    Mode: `mode`, CPOL << 1 | CPHA, 0 to 3, declares the SPI mode the device works in. At the
    first frame the device exchanges after each chip select edge or transfer start, the SPI
    model compares it with the firmware's CFG2.CPOL and CPHA. A mismatch ends the run with
    `ConfigError` naming both modes and every mismatching device of that frame, after the frame.
    A resumed or restored run is not refused again for that transfer. None, the default, is
    never checked. The mode is kept natively at construction, the check takes no GIL.

    Projects: a class used in `Device(cls, cs=...)` takes `cs` as a keyword argument and passes
    it to `super().__init__`, and `mode` the same way when it declares one.

    Time, state, reset, board and lifetime: as `I2CTarget`.
    """

    def __init__(self, cs: str | None = None, *, mode: int | None = None) -> None:
        """Select the device through GPIO pin `cs`, or the hardware SS output when None, and
        check the firmware's SPI mode against `mode` when not None.

        Raises:
            TypeError: `mode` is no int or None.
            ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15, or `mode` is outside
                0 to 3.
        """
        port, pin = chip_select(cs)
        checked = spi_mode("SpiTarget", mode)
        super().__init__(checked)
        self._cs = cs
        self._cs_port = port
        self._cs_pin = pin

    @property
    def cs(self) -> str | None:
        """The chip select pin, None for the hardware SS output."""
        return self._cs

    @property
    def board(self) -> DeviceBoard:
        """What the device may drive on the board of the machine it serves."""
        return DeviceBoard(self)

    def on_select(self, selected: bool, at: VirtualTime) -> None:
        """The chip select changed, `selected` when it went low."""

    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        """Exchange one frame of `bits` bits, 4 to 32: `out` from the controller, the frame
        returned to it, both in wire order, the first bit on the wire in bit `bits - 1`. All ones
        by default, an idle MISO."""
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


class SpiController(_core.SpiControllerBase):
    """A controller on an SPI bus, outside the MCU, clocking an SPI the firmware runs in target
    mode, CFG2.MASTER 0. Written in Python.

    Attach it with `Machine(devices={"SPI1": [SpiController(sck_hz=1_000_000)]})` or a machine
    description's `Periph(devices=[Device(SpiController, sck_hz=...)])`, alone on its SPI: no
    `SpiTarget` and no second controller on that instance.

    `transfer` selects the SPI's NSS input, shifts its frames one frame time apart, `bits` bits
    each at `sck_hz`, and deselects. NSS is driven directly, not through a GPIO pin: with
    CFG2.SSM 1 the firmware's CR1.SSI decides, as on silicon. The firmware's SPI returns one
    frame per frame sent: the TX FIFO head, its underrun pattern, or all ones when it is not
    selected or not in target mode. `on_frame` hears each frame at its end.

    Frame format: frames cross in wire order, the first bit on the wire in bit `bits - 1`, both
    ways, as for `SpiTarget`. With CFG2.LSBFRST 1 the firmware reads each frame bit reversed
    over its DSIZE + 1 bits, and the controller hears the firmware's frames reversed.

    Mode: `mode`, CPOL << 1 | CPHA, 0 to 3, declares the SPI mode the controller clocks in. At
    the first frame of each transfer the firmware's SPI takes part in, the SPI model compares it
    with CFG2.CPOL and CPHA. A mismatch ends the run with `ConfigError` naming both modes, at
    the start of that frame, before its `on_frame`. A resumed or restored run is not refused
    again for that transfer. None, the default, is never checked. The mode is kept natively at
    construction, the check takes no GIL.

    Time: `at` is the machine time since reset, as `Machine.time` and `transfer(at=)`, not the
    time since power-on an `SpiTarget` or `I2CTarget` hook gets: after a reset other than
    POWER_ON the two differ by the time before that reset.

    Reset: a reset of the machine drops the transfers not finished, the frames not yet shifted
    included, and `on_frame` is not called for them. `received` then holds fewer frames than were
    queued.

    State: the queued transfers are the SPI model's state, saved with a snapshot. `received`
    belongs to the test: a snapshot does not save it and a restore does not change it.

    Lifetime: as `I2CTarget`.
    """

    received: list[int]
    """The frames the firmware sent, in order, as the default `on_frame` appends them. Frames a
    reset dropped are missing."""

    def __init__(self, *, sck_hz: int, bits: int = 8, mode: int | None = None) -> None:
        """Clock frames of `bits` bits at `sck_hz`, checking the firmware's SPI mode against
        `mode` when not None.

        Raises:
            TypeError: `sck_hz` or `bits` is no int, or `mode` no int or None.
            ValueError: `sck_hz` is outside 1 to 100 MHz, `bits` outside 4 to 32, or `mode`
                outside 0 to 3.
        """
        for name, value in (("sck_hz", sck_hz), ("bits", bits)):
            if isinstance(value, bool) or not isinstance(cast(object, value), int):
                raise TypeError(f"SpiController takes {name} as an int, not {value!r}")
        if not 1 <= sck_hz <= _MAX_SCK_HZ:
            raise ValueError(f"an SPI clock is 1 Hz to 100 MHz, not {sck_hz} Hz")
        if not 4 <= bits <= 32:
            raise ValueError(f"an SPI frame is 4 to 32 bits, not {bits}")
        checked = spi_mode("SpiController", mode)
        super().__init__(checked)
        self._sck_hz = sck_hz
        self._bits = bits
        self.received = []

    @property
    def sck_hz(self) -> int:
        """The SCK frequency in Hz the controller clocks frames at, 1 to 100 MHz. Read-only."""
        return self._sck_hz

    @property
    def bits(self) -> int:
        """The size in bits of each frame the controller shifts, 4 to 32. Read-only."""
        return self._bits

    def transfer(
        self, frames: Sequence[int], *, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Shift `frames`, in wire order, into the SPI the controller serves, posted as one
        command as `UartStream.write` is: at once while the machine is parked, at the next block
        boundary while it runs. The transfer starts at exactly `at`, or at the time the command
        runs with `at` None, which depends on that boundary while running, and in both cases not
        before the end of the transfers queued before it. Each frame lasts
        `bits * 10**12 // sck_hz` ps. `at` and the start are machine times since reset. A reset
        of the machine drops the transfer if it has not finished. Await the result for the
        start.

        Raises:
            TypeError: `frames` is no sequence of ints, or `at` no `VirtualTime`.
            ValueError: `frames` holds no frame or more than 4096, a frame is outside 0 to
                2**bits - 1, or `at` is negative or past 2**64 - 1 ps. From the await, `at` is
                already past when the command runs, or the transfer ends past 2**64 - 1 ps.
            ConfigError: The controller serves no live machine. From the await, the transfers
                queued and not finished would hold more than 4096 frames.
            RuntimeError: Called from a handler, a hook or a model.
        """
        if isinstance(frames, str | bytes | bytearray) or not isinstance(
            cast(object, frames), Sequence
        ):
            raise TypeError(f"transfer() takes a sequence of ints, not {frames!r}")
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"transfer() takes at as a VirtualTime or None, not {at!r}")
        if not 1 <= len(frames) <= _MAX_FRAMES:
            raise ValueError(f"an SPI transfer has 1 to 4096 frames, not {len(frames)}")
        checked: list[int] = []
        for frame in frames:
            if isinstance(frame, bool) or not isinstance(cast(object, frame), int):
                raise TypeError(f"an SPI frame is an int, not {frame!r}")
            if not 0 <= frame < 1 << self._bits:
                raise ValueError(f"a {self._bits}-bit SPI frame is 0 to {(1 << self._bits) - 1}")
            checked.append(frame)
        if at is not None and not 0 <= at.ps <= _MAX_PS:
            raise ValueError(f"an SPI transfer at {at} leaves the 64-bit picosecond range")
        held = _ATTACHED.get(id(self))
        post = None if held is None else held[3]()
        if held is None or post is None:
            raise ConfigError(
                f"{self!r} serves no live machine",
                key="devices",
                expected="a controller serving a live machine",
            )
        return post(held[5], at, self._bits * 10**12 // self._sck_hz, self._bits, checked)

    def on_frame(self, sent: int, received: int, at: VirtualTime) -> None:
        """One frame ended at `at`, machine time since reset: `sent` the controller's frame,
        `received` the frame the firmware sent, all ones when its SPI did not take part, both in
        wire order.
        Appends `received` to `received` by default. Called on the machine's thread with the
        GIL, once per frame."""
        self.received.append(received)

    def _frame(self, sent: int, received: int, at: int) -> None:
        self.on_frame(sent, received, VirtualTime(at))

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._sck_hz} Hz, {self._bits} bits>"


@dataclass(frozen=True, slots=True)
class I2CAnswer:
    """One phase an `I2CController` queued, as the MCU answered it.

    Attributes:
        unit: The number the phase was queued with, increasing in queuing order on the I2C.
        phase: The phase, an address, a written byte or a read byte.
        ack: Whether the MCU acknowledged the address or the written byte. For a read byte, the
            acknowledge the controller gave it.
        byte: The byte the MCU sent in a read phase, 0xFF for the other phases.
        error: The error the phase ended with, None for none. None for a phase the MCU answers
            itself.
        at: Machine time since reset at which SCL was released after the phase.
    """

    unit: int
    phase: I2CPhase
    ack: bool
    byte: int
    error: I2CError | None
    at: VirtualTime


class I2CController(_core.I2CControllerBase):
    """A controller on an I2C bus, outside the MCU, addressing an I2C the firmware runs in
    target mode, with OA1EN, OA2EN or GCEN. Written in Python.

    Attach it with `Machine(devices={"I2C1": [I2CController()]})` or a machine description's
    `Periph(devices=[Device(I2CController, scl_hz=...)])`, alone on its I2C: no `I2CTarget` and
    no second controller on that instance.

    Phases: `start`, `write`, `read` and `stop` queue a START with its address byte, written
    bytes, read bytes and a STOP. Each START, write and read phase lasts nine SCL periods, eight
    bits and the acknowledge, and starts at the latest of its `at`, the end of every phase
    queued before it and the time of every STOP queued before it. A target that stretches SCL,
    with ADDR, RXNE or TXIS pending, holds the phase after it and every STOP until it releases
    SCL. A START with no STOP before it is a repeated START.

    STOP: `stop` takes effect at its own time. A STOP due while the target stretches SCL runs
    after the stretched byte's answer. A phase still queued or running at the STOP otherwise is
    dropped, as on a bus where the controller sends STOP in the middle of a byte, so a script
    queues the STOP at or after the end of the last phase, or after its answer.

    Answers: `on_answer` hears one `I2CAnswer` per START, write and read phase, at the time SCL
    is released after it, on the machine's thread with the GIL. A phase the I2C drops, by a
    reset of the MCU or of the I2C, by a STOP due before its end or with no acknowledged
    address, is answered with the undriven reply, NACK for an address or a written byte, 0xFF
    with ack False for a read byte, at the time the I2C reports it. So every phase gets exactly
    one answer. A STOP gets none.

    Time: `at` is the machine time since reset, as `Machine.time`, not the time since power-on
    an `I2CTarget` hook gets.

    State: the queued phases are the I2C model's state, saved with a snapshot. `answers`
    belongs to the test: a snapshot does not save it and a restore does not change it.

    Lifetime: as `I2CTarget`.
    """

    answers: list[I2CAnswer]
    """The answers heard, in order, as the default `on_answer` appends them."""

    def __init__(self, *, scl_hz: int = 100_000) -> None:
        """Clock phases at `scl_hz`.

        Raises:
            TypeError: `scl_hz` is no int.
            ValueError: `scl_hz` is outside 1 kHz to 5 MHz.
        """
        if isinstance(scl_hz, bool) or not isinstance(cast(object, scl_hz), int):
            raise TypeError(f"I2CController takes scl_hz as an int, not {scl_hz!r}")
        if not _MIN_SCL_HZ <= scl_hz <= _MAX_SCL_HZ:
            raise ValueError(f"an I2C clock is 1 kHz to 5 MHz, not {scl_hz} Hz")
        super().__init__()
        self._scl_hz = scl_hz
        self.answers = []

    @property
    def scl_hz(self) -> int:
        """The SCL frequency in Hz the controller clocks phases at, 1 kHz to 5 MHz. Read-only."""
        return self._scl_hz

    @property
    def phase_time(self) -> VirtualDuration:
        """The length of one START, write or read phase, `9 * 10**12 // scl_hz` ps."""
        return VirtualDuration(self._phase_ps)

    @property
    def _phase_ps(self) -> int:
        return 9 * 10**12 // self._scl_hz

    def start(
        self, address: int, *, read: bool = False, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue a START, or a repeated START when no `stop` came before it, and the address
        byte of `address`, a read transfer when `read`. Posted as one command as
        `UartStream.write` is: at once while the machine is parked, at the next block boundary
        while it runs. `at` is the earliest start, the time the command runs when None, which
        depends on that boundary while running. Await the result for that earliest start.

        Raises:
            TypeError: `address` is no int, `read` no bool, or `at` no `VirtualTime`.
            ValueError: `address` is outside 0 to 0x7F, or `at` outside 0 to 2**63 ps. From the
                await, `at` is already past when the command runs.
            ConfigError: The controller serves no live machine. From the await, the I2C would
                hold more than 4096 queued phases and STOPs.
            RuntimeError: Called from a handler, a hook or a model.
        """
        if isinstance(address, bool) or not isinstance(cast(object, address), int):
            raise TypeError(f"start() takes an address as an int, not {address!r}")
        if not 0 <= address <= 0x7F:
            raise ValueError(f"an I2C address has 7 bits, 0 to 0x7F, not {address:#x}")
        if not isinstance(cast(object, read), bool):
            raise TypeError(f"start() takes read as a bool, not {read!r}")
        return self._queue([_START], [address << 1 | int(read)], at)

    def write(
        self, data: bytes | Sequence[int], *, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue one write phase per byte of `data`, as `start` posts and places them.

        Raises:
            TypeError: `data` is no bytes or sequence of ints, or `at` no `VirtualTime`.
            ValueError: `data` holds no byte or more than 4096, a byte is outside 0 to 0xFF,
                or `at` is outside 0 to 2**63 ps. From the await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        if isinstance(data, str) or not isinstance(
            cast(object, data), bytes | bytearray | Sequence
        ):
            raise TypeError(f"write() takes bytes or a sequence of ints, not {data!r}")
        if not 1 <= len(data) <= _MAX_PHASES:
            raise ValueError(f"an I2C write has 1 to 4096 bytes, not {len(data)}")
        checked: list[int] = []
        for byte in data:
            if isinstance(byte, bool) or not isinstance(cast(object, byte), int):
                raise TypeError(f"an I2C byte is an int, not {byte!r}")
            if not 0 <= byte <= 0xFF:
                raise ValueError(f"an I2C byte is 0 to 0xFF, not {byte}")
            checked.append(byte)
        return self._queue([_WRITE] * len(checked), checked, at)

    def read(
        self, count: int, *, ack_last: bool = False, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue `count` read phases, as `start` posts and places them, the controller
        acknowledging every byte but the last, and the last when `ack_last`.

        Raises:
            TypeError: `count` is no int, `ack_last` no bool, or `at` no `VirtualTime`.
            ValueError: `count` is outside 1 to 4096, or `at` outside 0 to 2**63 ps. From the
                await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        if isinstance(count, bool) or not isinstance(cast(object, count), int):
            raise TypeError(f"read() takes a count as an int, not {count!r}")
        if not 1 <= count <= _MAX_PHASES:
            raise ValueError(f"an I2C read has 1 to 4096 bytes, not {count}")
        if not isinstance(cast(object, ack_last), bool):
            raise TypeError(f"read() takes ack_last as a bool, not {ack_last!r}")
        return self._queue([_READ] * count, [1] * (count - 1) + [int(ack_last)], at)

    def stop(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue a STOP at `at`, the time the command runs when None, posted as `start` posts.
        Every phase queued before it and still queued or running at its time is dropped and
        answered undriven. A STOP due while the target stretches SCL runs after the stretched
        byte's answer. Await the result for the STOP's time.

        Raises:
            TypeError: `at` is no `VirtualTime`.
            ValueError: `at` is outside 0 to 2**63 ps. From the await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        return self._queue([_STOP], [0], at)

    def on_answer(self, answer: I2CAnswer) -> None:
        """One phase was answered, called on the machine's thread with the GIL, once per
        phase. Appends `answer` to `answers` by default."""
        self.answers.append(answer)

    def _queue(
        self, kinds: list[int], values: list[int], at: VirtualTime | None
    ) -> Pending[VirtualTime]:
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"an I2C phase takes at as a VirtualTime or None, not {at!r}")
        if at is not None and not 0 <= at.ps <= _MAX_I2C_AT_PS:
            raise ValueError(f"an I2C phase at {at} lies outside 0 to 2**63 ps")
        held = _ATTACHED.get(id(self))
        post = None if held is None else held[4]()
        if held is None or post is None:
            raise ConfigError(
                f"{self!r} serves no live machine",
                key="devices",
                expected="a controller serving a live machine",
            )
        return post(held[5], at, self._phase_ps, kinds, values)

    def _answered(self, unit: int, phase: int, ack: bool, byte: int, error: int, at: int) -> None:
        self.on_answer(
            I2CAnswer(
                unit,
                I2CPhase(phase),
                ack,
                byte,
                I2CError(error) if error else None,
                VirtualTime(at),
            )
        )

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._scl_hz} Hz>"


def serving(device: BusDevice) -> object | None:
    """The live machine `device` serves, None when it serves none."""
    held = _ATTACHED.get(id(device))
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


def attach(
    device: BusDevice,
    machine: object,
    audio_clock: AudioClock,
    bus_hold: BusHold,
    spi_transfer: SpiTransfer,
    i2c_phases: I2CPhases,
    peripheral: str,
) -> None:
    """Mark `device` as serving `machine` on `peripheral`, `audio_clock` a method of `machine`
    its `DeviceBoard` calls, `bus_hold` one `I2CTarget.hold_bus` calls, `spi_transfer` one
    `SpiController.transfer` calls, `i2c_phases` one the `I2CController` phase methods call. The
    mark is weak: once that machine is gone the device may serve another one. Keyed by
    identity, so a device defining `__eq__` works."""
    key = id(device)
    if key not in _ATTACHED:
        weakref.finalize(device, _ATTACHED.pop, key, None)
    _ATTACHED[key] = (
        weakref.ref(machine),
        weakref.WeakMethod(audio_clock),
        weakref.WeakMethod(bus_hold),
        weakref.WeakMethod(spi_transfer),
        weakref.WeakMethod(i2c_phases),
        peripheral,
    )

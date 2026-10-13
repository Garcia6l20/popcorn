"""Devices on the I2C and SPI buses of the MCU, written in Python: `popcorn.board.I2CTarget`,
`popcorn.board.SpiTarget` and `popcorn.board.SpiController`, served by the native bus models."""

from __future__ import annotations

import re
import typing
import weakref
from collections.abc import Callable, Sequence
from typing import cast

from popcorn import _core
from popcorn._enums import I2CError, ResetKind
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


type BusDevice = _core.I2CTargetBase | _core.SpiTargetBase | _core.SpiControllerBase
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

type _Attachment = tuple[
    weakref.ReferenceType[object],
    weakref.WeakMethod[AudioClock],
    weakref.WeakMethod[BusHold],
    weakref.WeakMethod[SpiTransfer],
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


def cs_of(device: _core.SpiTargetBase) -> str | None:
    """The chip select pin name of an SPI device, None for the hardware SS output."""
    return typing.cast("str | None", getattr(device, "cs", None))


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
    target never knows a transfer's length in advance. Of two targets answering one address, the
    first attached answers.

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
        """A STOP ended the transfer the target took part in."""

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
        return hold(held[4], at, for_)

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
    hardware SS output, from the start to the end of each transfer.

    Projects: a class used in `Device(cls, cs=...)` takes `cs` as a keyword argument and passes
    it to `super().__init__`.

    Time, state, reset, board and lifetime: as `I2CTarget`.
    """

    def __init__(self, cs: str | None = None) -> None:
        """Select the device through GPIO pin `cs`, or the hardware SS output when None.

        Raises:
            ValueError: `cs` is not a pin name such as "PA4", pin 0 to 15.
        """
        port, pin = chip_select(cs)
        super().__init__()
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

    def __init__(self, *, sck_hz: int, bits: int = 8) -> None:
        """Clock frames of `bits` bits at `sck_hz`.

        Raises:
            TypeError: `sck_hz` or `bits` is no int.
            ValueError: `sck_hz` is outside 1 to 100 MHz, or `bits` outside 4 to 32.
        """
        for name, value in (("sck_hz", sck_hz), ("bits", bits)):
            if isinstance(value, bool) or not isinstance(cast(object, value), int):
                raise TypeError(f"SpiController takes {name} as an int, not {value!r}")
        if not 1 <= sck_hz <= _MAX_SCK_HZ:
            raise ValueError(f"an SPI clock is 1 Hz to 100 MHz, not {sck_hz} Hz")
        if not 4 <= bits <= 32:
            raise ValueError(f"an SPI frame is 4 to 32 bits, not {bits}")
        super().__init__()
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
        """Shift `frames` into the SPI the controller serves, posted as one command as
        `UartStream.write` is: at once while the machine is parked, at the next block boundary
        while it runs. The transfer starts at exactly `at`, or at the time the command runs with
        `at` None, which depends on that boundary while running, and in both cases not before
        the end of the transfers queued before it. Each frame lasts `bits * 10**12 // sck_hz`
        ps. `at` and the start are machine times since reset. A reset of the machine drops the
        transfer if it has not finished. Await the result for the start.

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
        return post(held[4], at, self._bits * 10**12 // self._sck_hz, self._bits, checked)

    def on_frame(self, sent: int, received: int, at: VirtualTime) -> None:
        """One frame ended at `at`, machine time since reset: `sent` the controller's frame,
        `received` the frame the firmware sent, all ones when its SPI did not take part.
        Appends `received` to `received` by default. Called on the machine's thread with the
        GIL, once per frame."""
        self.received.append(received)

    def _frame(self, sent: int, received: int, at: int) -> None:
        self.on_frame(sent, received, VirtualTime(at))

    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self._sck_hz} Hz, {self._bits} bits>"


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
    peripheral: str,
) -> None:
    """Mark `device` as serving `machine` on `peripheral`, `audio_clock` a method of `machine`
    its `DeviceBoard` calls, `bus_hold` one `I2CTarget.hold_bus` calls, `spi_transfer` one
    `SpiController.transfer` calls. The mark is weak: once
    that machine is gone the device may serve another one. Keyed by identity, so a device
    defining `__eq__` works."""
    key = id(device)
    if key not in _ATTACHED:
        weakref.finalize(device, _ATTACHED.pop, key, None)
    _ATTACHED[key] = (
        weakref.ref(machine),
        weakref.WeakMethod(audio_clock),
        weakref.WeakMethod(bus_hold),
        weakref.WeakMethod(spi_transfer),
        peripheral,
    )

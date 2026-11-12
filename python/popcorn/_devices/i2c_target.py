from __future__ import annotations

import typing
from typing import cast, override

from popcorn import _core
from popcorn._devices.base import (
    ATTACHED,
    load_trampoline,
    read_reply,
    reply,
    reset_trampoline,
    restore_state,
    save_trampoline,
    snapshot_state,
)
from popcorn._devices.naming import check_address, refuse_reserved
from popcorn._devices.pins import DeviceBoard
from popcorn._enums import I2CError, ResetKind
from popcorn._errors import ConfigError
from popcorn._limits import MAX_PS
from popcorn.units import VirtualDuration, VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._refs import Pending

_MAX_STRETCH_PS = 10**12


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

    Raising: an exception from `on_start`, `on_write`, `on_read`, `on_stop`, `reset` or `restore`
    stops the run with that exception, once the bus phase or the reset has finished for every
    target. The bus model goes on with the phase answered as by an absent target: a NACK for
    `on_start` and `on_write`, a byte of all ones for `on_read`. After a raising `on_write` the
    transfer ends with a NACK and a STOP. The next run continues from there.

    Lifetime: a target serves one machine at a time and must not keep a reference to it. It may
    keep its `board`, which holds no machine.

    Errors: a hook returning an `I2CError` makes the byte it answers end with that error. The I2C
    model sets the matching ISR flag at the end of that byte and raises the error IRQ when ERRIE
    is set. Nothing is armed for a later byte. `BUS_ERROR` and `OVERRUN` leave the transfer
    running, the byte acknowledged, a read byte 0xFF. `ARBITRATION_LOST` ends the transfer for the
    controller: CR2.START cleared, no NACKF, TC or STOPF, BUSY 1 for one STOP length, then
    `on_stop`, which a PE clear inside that window delivers at the time of the write. Raising from
    a hook is not an injection: it stops the run as a handler error.

    Names: a name starting with `_` that this class defines is reserved. A subclass defining one
    raises TypeError at class definition, and an instance attribute with one raises TypeError when
    the machine takes the device. Other `_` names are free for the subclass, and an attribute a
    subclass sets never replaces the class's own state, which is name-mangled.
    """

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        refuse_reserved(cls, I2CTarget)

    def __init__(self, address: int) -> None:
        """Answer `address`.

        Raises:
            ValueError: `address` is not a 7-bit target address, 0x01 to 0x77.
        """
        check_address(address)
        super().__init__()
        self.__bus_address = address
        self.__stretch_ps: int | None = None

    @property
    def address(self) -> int:
        """The 7-bit address the target answers."""
        return self.__bus_address

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
        if self.__stretch_ps is None:
            raise RuntimeError(
                f"{self!r}: stretch() is called from on_start, on_write or on_read only"
            )
        self.__stretch_ps = for_.ps

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
        if for_.ps > MAX_PS:
            raise ValueError(f"a bus hold of {for_} leaves the 64-bit picosecond range")
        if at is not None and not 0 <= at.ps <= MAX_PS - for_.ps:
            raise ValueError(f"a bus hold from {at} for {for_} leaves the 64-bit picosecond range")
        held = ATTACHED.get(id(self))
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
        return snapshot_state(self, I2CTarget)

    def restore(self, data: bytes) -> None:
        """Set the declared state fields from bytes `snapshot()` returned."""
        restore_state(self, I2CTarget, data)

    def _address(self) -> int:
        return self.__bus_address

    def _start(self, read: bool, at: int) -> tuple[bool, int, int]:
        self.__stretch_ps = 0
        try:
            return reply(self, "on_start", self.on_start(read, VirtualTime(at)), self._stretched())
        finally:
            self.__stretch_ps = None

    def _write(self, byte: int, at: int) -> tuple[bool, int, int]:
        self.__stretch_ps = 0
        try:
            return reply(self, "on_write", self.on_write(byte, VirtualTime(at)), self._stretched())
        finally:
            self.__stretch_ps = None

    def _read(self, at: int) -> tuple[int, int, int]:
        self.__stretch_ps = 0
        try:
            return read_reply(self, "on_read", self.on_read(VirtualTime(at)), self._stretched())
        finally:
            self.__stretch_ps = None

    def _stretched(self) -> int:
        return self.__stretch_ps or 0

    def _stop(self, at: int) -> None:
        self.on_stop(VirtualTime(at))

    _reset = reset_trampoline
    _save = save_trampoline
    _load = load_trampoline

    @override
    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self.__bus_address:#04x}>"

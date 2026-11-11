from __future__ import annotations

import typing
from typing import override

from popcorn import _core
from popcorn._devices.base import (
    load_trampoline,
    reset_trampoline,
    restore_state,
    save_trampoline,
    snapshot_state,
    word,
)
from popcorn._devices.naming import chip_select, refuse_reserved, spi_mode
from popcorn._devices.pins import DeviceBoard
from popcorn._enums import ResetKind
from popcorn.units import VirtualTime

if typing.TYPE_CHECKING:
    pass

_SPI_TARGET_ATTRIBUTES = frozenset({"_cs_pin", "_cs_port"})


class SpiTarget(_core.SpiTargetBase):
    """A device on an SPI bus, written in Python.

    Attach it with `Machine(devices={"SPI1": [device]})` or a machine description's
    `Periph(devices=[Device(cls, cs=...)])`. The SPI model calls `on_select` on each chip select
    edge and `on_exchange` for each frame while the device is selected, full duplex. Devices
    sharing a chip select, or several on the hardware SS output, are all selected and see every
    frame, their replies ANDed as on an open-drain MISO.

    Raising: an exception from `on_exchange`, `on_select`, `reset` or `restore` stops the run with
    that exception, once the frame has landed or the reset has finished for every device. The
    frame lands as if the device were absent, with the bits it would drive high, and the next run
    continues the transfer. A device that raised misses that frame, so one that parses a command
    byte answers the rest of the transfer out of step.

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

    Names: a name starting with `_` that this class defines is reserved, as are `_cs_pin` and
    `_cs_port`, which the SPI model reads. A subclass defining one raises TypeError at class
    definition. An instance attribute named like one of the class's methods raises TypeError when
    the machine takes the device, while `_cs_pin` and `_cs_port` may be set on the instance.
    Other `_` names are free for the subclass, and an attribute a subclass sets never replaces the
    class's own state, which is name-mangled.
    """

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        refuse_reserved(cls, SpiTarget, _SPI_TARGET_ATTRIBUTES)

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
        self.__cs = cs
        self._cs_port = port
        self._cs_pin = pin

    @property
    def cs(self) -> str | None:
        """The chip select pin, None for the hardware SS output."""
        return self.__cs

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
        return snapshot_state(self, SpiTarget)

    def restore(self, data: bytes) -> None:
        """Set the declared state fields from bytes `snapshot()` returned."""
        restore_state(self, SpiTarget, data)

    def _select(self, selected: bool, at: int) -> None:
        self.on_select(selected, VirtualTime(at))

    def _exchange(self, out: int, bits: int, at: int) -> int:
        return word(self, "on_exchange", self.on_exchange(out, bits, VirtualTime(at)), bits)

    _reset = reset_trampoline
    _save = save_trampoline
    _load = load_trampoline

    @override
    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} on {self.__cs or 'SS'}>"

from __future__ import annotations

import typing
from collections.abc import Sequence
from typing import cast, override

from popcorn import _core
from popcorn._devices.base import ATTACHED
from popcorn._devices.naming import refuse_reserved, spi_mode
from popcorn._errors import ConfigError
from popcorn._limits import MAX_PS
from popcorn.units import VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._refs import Pending

_MAX_SCK_HZ = 100_000_000
_MAX_FRAMES = 4096


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

    Names: a name starting with `_` that this class defines is reserved. A subclass defining one
    raises TypeError at class definition, and an instance attribute with one raises TypeError when
    the machine takes the device. Other `_` names are free for the subclass, and an attribute a
    subclass sets never replaces the class's own state, which is name-mangled.
    """

    received: list[int]
    """The frames the firmware sent, in order, as the default `on_frame` appends them. Frames a
    reset dropped are missing."""

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        refuse_reserved(cls, SpiController)

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
        self.__sck_hz = sck_hz
        self.__bits = bits
        self.received = []

    @property
    def sck_hz(self) -> int:
        """The SCK frequency in Hz the controller clocks frames at, 1 to 100 MHz. Read-only."""
        return self.__sck_hz

    @property
    def bits(self) -> int:
        """The size in bits of each frame the controller shifts, 4 to 32. Read-only."""
        return self.__bits

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
            if not 0 <= frame < 1 << self.__bits:
                raise ValueError(f"a {self.__bits}-bit SPI frame is 0 to {(1 << self.__bits) - 1}")
            checked.append(frame)
        if at is not None and not 0 <= at.ps <= MAX_PS:
            raise ValueError(f"an SPI transfer at {at} leaves the 64-bit picosecond range")
        held = ATTACHED.get(id(self))
        post = None if held is None else held[3]()
        if held is None or post is None:
            raise ConfigError(
                f"{self!r} serves no live machine",
                key="devices",
                expected="a controller serving a live machine",
            )
        return post(held[5], at, self.__bits * 10**12 // self.__sck_hz, self.__bits, checked)

    def on_frame(self, sent: int, received: int, at: VirtualTime) -> None:
        """One frame ended at `at`, machine time since reset: `sent` the controller's frame,
        `received` the frame the firmware sent, all ones when its SPI did not take part, both in
        wire order.
        Appends `received` to `received` by default. Called on the machine's thread with the
        GIL, once per frame."""
        self.received.append(received)

    def _frame(self, sent: int, received: int, at: int) -> None:
        self.on_frame(sent, received, VirtualTime(at))

    @override
    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self.__sck_hz} Hz, {self.__bits} bits>"

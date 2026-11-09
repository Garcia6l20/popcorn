"""A three-axis accelerometer on SPI, written in Python as a `popcorn.board.SpiTarget`.

The register map follows the common LIS3DH layout. A transfer is one chip select period of 8-bit
frames in SPI mode 3. The first frame is the command: bit 7 reads, bit 6 increments the address
after each data frame, bits 5 to 0 the address. Each later frame reads or writes one register.
WHO_AM_I at 0x0F answers 0x33. OUT_X_L to OUT_Z_H, 0x28 to 0x2D, hold each axis as a 16-bit
little-endian value.
"""

from typing import override

from popcorn import ResetKind
from popcorn.board import SpiTarget
from popcorn.units import VirtualTime, ms

READ = 0x80
INCREMENT = 0x40
ADDRESS_MASK = 0x3F
WHO_AM_I = 0x0F
IDENTITY = 0x33
CTRL_REG1 = 0x20
OUT_X_L = 0x28
AXES = 3
ONE_G = 16384
MODE = 3


def axes_at(counts_per_ms: int, at: VirtualTime) -> tuple[int, int, int]:
    """The X, Y and Z readings at @p at: X a ramp, Y its negative, Z one g."""
    ramp = at.ps * counts_per_ms // ms(1).ps % ONE_G
    return ramp, -ramp, ONE_G


class Accelerometer(SpiTarget):
    """An SPI accelerometer whose X axis rises by `counts_per_ms` counts per millisecond.

    A command frame latches the three axes, so one transfer reads one consistent sample. The
    ramp slope is fixed when the device is built and read-only, because the `spi` fixture reuses
    one device for every test. The annotated fields are the state a snapshot keeps: the control
    register, the counters and the transfer in progress.
    """

    ctrl_reg1: int
    selects: int
    samples: int
    address: int
    reading: bool
    increment: bool
    frame: int
    latched: bytes

    def __init__(self, cs: str | None = None, counts_per_ms: int = 100, mode: int = MODE) -> None:
        """Select the accelerometer through `cs` in SPI `mode`."""
        super().__init__(cs, mode=mode)
        self._counts_per_ms = counts_per_ms
        self.ctrl_reg1 = 0
        self.selects = 0
        self.samples = 0
        self.address = 0
        self.reading = False
        self.increment = False
        self.frame = 0
        self.latched = bytes(2 * AXES)

    @property
    def counts_per_ms(self) -> int:
        """The slope of the X ramp."""
        return self._counts_per_ms

    @override
    def reset(self, kind: ResetKind) -> None:
        """Clear the control register on POWER_ON and BROWN_OUT. The counters stay."""
        if kind in (ResetKind.POWER_ON, ResetKind.BROWN_OUT):
            self.ctrl_reg1 = 0
            self.frame = 0

    @override
    def on_select(self, selected: bool, at: VirtualTime) -> None:
        if selected:
            self.selects += 1
        self.frame = 0

    @override
    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        self.frame += 1
        if self.frame == 1:
            self.reading = bool(out & READ)
            self.increment = bool(out & INCREMENT)
            self.address = out & ADDRESS_MASK
            if self.reading and self.address == OUT_X_L:
                self.samples += 1
                self.latched = b"".join(
                    (value & 0xFFFF).to_bytes(2, "little")
                    for value in axes_at(self._counts_per_ms, at)
                )
            return 0
        address = self.address
        if self.increment:
            self.address = (self.address + 1) & ADDRESS_MASK
        if not self.reading:
            if address == CTRL_REG1:
                self.ctrl_reg1 = out
            return 0
        if address == WHO_AM_I:
            return IDENTITY
        if address == CTRL_REG1:
            return self.ctrl_reg1
        if OUT_X_L <= address < OUT_X_L + 2 * AXES:
            return self.latched[address - OUT_X_L]
        return 0

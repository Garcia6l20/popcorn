"""A two-channel 12-bit ADC on SPI, written in Python as a `popcorn.board.SpiTarget`.

A conversion is one chip select period of two 16-bit frames. The first frame carries the
command, bit 15 the start bit and bit 12 the channel, while the ADC answers 0. The second frame
returns the 12-bit sample, right-aligned.
"""

from collections.abc import Callable

from popcorn import ResetKind
from popcorn.board import SpiTarget
from popcorn.units import VirtualTime, ms

START = 0x8000
CHANNEL_BIT = 12
FRAME_BITS = 16
FULL_SCALE = 0xFFF

type Source = Callable[[int, VirtualTime], int]
"""The sample of a channel at a virtual time."""


class Adc(SpiTarget):
    """An SPI ADC answering samples from `source`, a function of the channel and the time.

    Without a source it answers a ramp of `counts_per_ms` counts per millisecond of virtual
    time on channel 0, and its mirror, full scale minus the ramp, on channel 1. Both are fixed
    when the ADC is built and read-only, because the `machine` fixture reuses one ADC for every
    test of a config. A test wanting another source builds its own machine with
    `Machine(devices={"SPI1": [Adc(cs="PB0", source=...)]})`.

    Framing: `selects` and `deselects` count chip select edges, `bad_frames` counts the
    frames of another size than 16 bits and the conversions whose chip select period did not
    hold exactly two frames. These and the conversion in progress are the annotated fields,
    the state a snapshot keeps.
    """

    selects: int
    deselects: int
    bad_frames: int
    frame: int
    channel: int

    def __init__(
        self, cs: str | None = None, counts_per_ms: int = 10, source: Source | None = None
    ) -> None:
        """Select the ADC through `cs`, answering `source` or the default ramp."""
        super().__init__(cs)
        self._counts_per_ms = counts_per_ms
        self._source = source if source is not None else self.ramp
        self.selects = 0
        self.deselects = 0
        self.bad_frames = 0
        self.frame = 0
        self.channel = 0

    @property
    def counts_per_ms(self) -> int:
        """The slope of the default ramp."""
        return self._counts_per_ms

    def ramp(self, channel: int, at: VirtualTime) -> int:
        """The default source: the ramp on channel 0, its mirror on channel 1."""
        rising = at.ps * self._counts_per_ms // ms(1).ps & FULL_SCALE
        return rising if channel == 0 else FULL_SCALE - rising

    def reset(self, kind: ResetKind) -> None:
        """Drop the conversion in progress on POWER_ON and BROWN_OUT. The counters stay."""
        if kind in (ResetKind.POWER_ON, ResetKind.BROWN_OUT):
            self.frame = 0
            self.channel = 0

    def on_select(self, selected: bool, at: VirtualTime) -> None:
        if selected:
            self.selects += 1
            self.frame = 0
            return
        self.deselects += 1
        if self.frame != 2:
            self.bad_frames += 1

    def on_exchange(self, out: int, bits: int, at: VirtualTime) -> int:
        self.frame += 1
        if bits != FRAME_BITS:
            self.bad_frames += 1
        if self.frame == 1:
            if out & START:
                self.channel = out >> CHANNEL_BIT & 1
            return 0
        if self.frame == 2:
            return self._source(self.channel, at) & FULL_SCALE
        return 0

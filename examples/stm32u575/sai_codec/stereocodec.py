"""A stereo audio codec on I2C, written in Python as a `popcorn.board.I2CTarget`.

The codec is the clock master of the audio bus: it drives the frame clock of SAI1 block A, a
slave. A write of 1 to its power register starts the frame clock at `FRAME_HZ`, a write of 0
stops it. The first byte of a write selects the register, the next ones write it. A read returns
the selected register.
"""

from popcorn import ResetKind
from popcorn.board import I2CTarget
from popcorn.units import VirtualTime

POWER = 0x02
FRAME_HZ = 48_000


class StereoCodec(I2CTarget):
    """A codec whose power register starts and stops the frame clock of SAI1 block A.

    The annotated fields are the state a snapshot keeps. `started_ps` and `stopped_ps` hold the
    virtual time of the last power write of each kind, 0 before the first one.
    """

    pointer: int
    power: int
    written: int
    started_ps: int
    stopped_ps: int

    def __init__(self, address: int) -> None:
        """Answer `address`, powered down."""
        super().__init__(address)
        self.pointer = 0
        self.power = 0
        self.written = 0
        self.started_ps = 0
        self.stopped_ps = 0

    def reset(self, kind: ResetKind) -> None:
        """Power down on POWER_ON and BROWN_OUT, as the board supply drops."""
        if kind in (ResetKind.POWER_ON, ResetKind.BROWN_OUT):
            self.pointer = 0
            self.power = 0
            self.written = 0

    def on_start(self, read: bool, at: VirtualTime) -> bool:
        self.written = 0
        return True

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        if self.written == 0:
            self.pointer = byte
        elif self.pointer == POWER:
            self.power = byte & 1
            self.board.set_audio_clock("SAI1", "A", FRAME_HZ if self.power else 0)
            if self.power:
                self.started_ps = at.ps
            else:
                self.stopped_ps = at.ps
        self.written += 1
        return True

    def on_read(self, at: VirtualTime) -> int:
        return self.power if self.pointer == POWER else 0

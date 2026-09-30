"""The RNG of the STM32U575 as a peripheral model written in Python, `popcorn.model.Model`.

popcorn has no native RNG model. This one serves the two registers a driver needs, CR and DR,
after RM0456 Rev 6 section 48. Setting RNGEN starts a conversion, which sets SR.DRDY one
`CONVERSION` later. A DR read while DRDY is set returns the next word of a seeded xorshift32
sequence, clears DRDY and starts the next conversion. Clearing RNGEN stops production and
keeps DRDY and DR valid (48.7.2, 48.7.3). A CR write with CONDRST set cancels the conversion
in progress and clears SR (48.7.1). The RNG interrupt line follows DRDY while CR.IE is set.
Every other RNG register keeps its described storage.

Simplifications: one word per conversion, where the RNG fills a four-word output buffer
(48.3.5). Not modelled: the seed and clock error flags SEIS and CEIS, which IE also covers on
silicon, and CED, whose value is ignored.
"""

from collections.abc import Iterator

from popcorn.model import IrqLine, Model, on_read, on_write
from popcorn.units import us

CONVERSION = us(40)
"""A round number, the initialization example of RM0456 48.5 at 160 MHz AHB and a 48 MHz RNG
clock. At this firmware's 4 MHz HCLK, 48.5 gives about 100 us per set of four words."""

NONE = -1


def xorshift32(state: int) -> int:
    """The word after `state` in the xorshift32 sequence."""
    state ^= state << 13 & 0xFFFFFFFF
    state ^= state >> 17
    state ^= state << 5 & 0xFFFFFFFF
    return state


def words(seed: int) -> Iterator[int]:
    """The words an `Rng` built with `seed` returns, in order."""
    state = seed
    while True:
        state = xorshift32(state)
        yield state


class Rng(Model):
    """A deterministic RNG: `seed` fixes the sequence, 1 to 2**32 - 1.

    The annotated fields are the state a snapshot saves and a reset restores: the generator
    position, the count of words read, and the handle of the conversion in progress, which
    `cancel()` takes. Event handles survive a snapshot, so a restored handle stays valid.
    """

    irq: IrqLine
    state: int
    served: int
    pending: int

    def __init__(self, seed: int = 1) -> None:
        """Start the sequence at `seed`.

        Raises:
            ValueError: `seed` is 0, which xorshift32 never leaves, or does not fit 32 bits.
        """
        super().__init__()
        if not 0 < seed < 1 << 32:
            raise ValueError(f"a seed is 1 to 2**32 - 1, not {seed}")
        self.state = seed
        self.served = 0
        self.pending = NONE

    @on_write("CR")
    def _cr(self, old: int, new: int) -> None:
        enable = self.fields.CR.RNGEN
        if new & self.fields.CR.CONDRST:
            self._stop()
            self.regs.SR = 0
        elif not new & enable:
            self._stop()
        elif not old & enable and not self.regs.SR & self.fields.SR.DRDY:
            self._start()
        self._follow()

    @on_read("DR")
    def _dr(self) -> int:
        if not self.regs.SR & self.fields.SR.DRDY:
            return 0
        self.state = xorshift32(self.state)
        self.served += 1
        self.regs.SR &= ~self.fields.SR.DRDY
        if self.regs.CR & self.fields.CR.RNGEN:
            self._start()
        self._follow()
        return self.state

    def _start(self) -> None:
        if self.pending == NONE:
            self.pending = self.schedule(CONVERSION, self._converted)

    def _stop(self) -> None:
        if self.pending != NONE:
            self.cancel(self.pending)
            self.pending = NONE

    def _converted(self) -> None:
        self.pending = NONE
        self.regs.SR |= self.fields.SR.DRDY
        self._follow()

    def _follow(self) -> None:
        if self.regs.SR & self.fields.SR.DRDY and self.regs.CR & self.fields.CR.IE:
            self.irq.raise_()
        else:
            self.irq.lower()

"""Sound activity detector of the ADF, written from RM0456 40.4.10 and 40.8.11 to 40.8.14.

Exact rational arithmetic with `Fraction`. The RM gives the averages, the slopes and the
Table 400 gains, not the fixed-point arithmetic, so these choices are popcorn's, not
silicon's:

- SDLVL is the mean of |PCM[23:8]| over the frame, rounded down, saturated to 15 bits.
- ANLVL keeps 12 fractional bits. Every value it takes, the LEARN mean included, is rounded
  down to a multiple of 2^-12 and saturated below 2^15. SADANLVR shows its integer part.
- Thresholds are compared exactly against the Table 400 gains, with no rounding.
- In SADMOD 00 the trigger is decided against the threshold of the ANLVL before the frame's
  update, and ANLVL is updated only when the frame does not trigger, then floored at ANMIN.
  SADMOD 1x updates ANLVL first and compares the new value. SADMOD 01 keeps the LEARN mean.
- Every mode starts with LEARN, Figure 380, and no frame of LEARN triggers.
- The hangover follows Figure 380: a frame under the threshold in DETECT leaves for MONITOR
  when the counter is 0, else decrements it, so HGOVR 4 frames leaves at the fifth.
- HYSTEN, which RM0456 forbids with SADMOD 1x, is ignored there.
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass
from enum import IntEnum
from fractions import Fraction

GAINS: tuple[Fraction, ...] = (
    Fraction(3, 2),
    Fraction(2),
    Fraction(3),
    Fraction(4),
    Fraction(6),
    Fraction(8),
    Fraction(12),
    Fraction(16),
    Fraction(24),
    Fraction(32),
)
"""THRH gain over LVL per SNTHR code, RM0456 Table 400. THRL of code n is THRH of n - 1."""

GAINS_DB: tuple[float, ...] = (3.5, 6.0, 9.5, 12.0, 15.6, 18.1, 21.6, 24.1, 27.6, 30.1)
"""THRH over LVL in dB per SNTHR code, as RM0456 Table 400 prints it."""

NOISE_STEP = Fraction(1, 4096)
NOISE_MAX = Fraction(2**27 - 1, 4096)
LEVEL_MAX = 0x7FFF


class SadState(IntEnum):
    """SADCR.SADST codes, RM0456 40.8.11."""

    LEARN = 0
    MONITOR = 1
    DETECT = 3


@dataclass(frozen=True)
class SadConfig:
    """SADCR and SADCFGR field codes, RM0456 40.8.11 and 40.8.12."""

    sadmod: int
    frsize: int
    lfrnb: int
    hgovr: int
    anslp: int
    snthr: int
    hysten: bool
    anmin: int

    @property
    def frame(self) -> int:
        """Samples per frame: 8 to 256 for codes 0 to 5, 512 for 11x."""
        return 8 * 2 ** min(self.frsize, 6)

    @property
    def learn_frames(self) -> int:
        """Frames of LEARN: 2 to 16 for codes 0 to 3, 32 for 1xx."""
        return 2 * 2 ** min(self.lfrnb, 4)

    @property
    def hangover(self) -> int:
        """Hangover frames, 4 x 2^HGOVR."""
        return 4 * 2**self.hgovr


@dataclass(frozen=True)
class SadFrame:
    """One frame: SDLVL, ANLVL's integer part, the state after it, whether it changed."""

    sdlvl: int
    anlvl: int
    state: SadState
    changed: bool


def msb16(pcm24: int) -> int:
    """PCM[23:8] of a signed 24-bit sample, as a signed 16-bit value."""
    top = (pcm24 & 0xFFFFFF) >> 8
    return top - 0x10000 if top & 0x8000 else top


def quantize(value: Fraction) -> Fraction:
    """Rounds down to the 2^-12 grid of ANLVL and saturates."""
    return min(Fraction(math.floor(value / NOISE_STEP)) * NOISE_STEP, NOISE_MAX)


def update_noise(noise: Fraction, sdlvl: int, anslp: int) -> Fraction:
    """ANLVL_DN when SDLVL is below it, else ANLVL_UP, RM0456 40.4.10."""
    down = quantize(noise * (1 - Fraction(2) ** (anslp - 10)))
    if sdlvl < down:
        return down
    return quantize(noise * (1 + Fraction(2) ** (anslp - 12)))


def sad(pcm24: Sequence[int], config: SadConfig) -> list[SadFrame]:
    """Runs the SAD from its enable over DFLT0 samples.

    Args:
        pcm24: Signed 24-bit DFLT0 output samples.
        config: Field codes, SNTHR 0 to 9.

    Returns:
        One entry per complete frame.
    """
    state = SadState.LEARN
    noise = Fraction(0)
    learned: list[int] = []
    hang = 0
    out: list[SadFrame] = []
    for start in range(0, len(pcm24) - config.frame + 1, config.frame):
        frame = pcm24[start : start + config.frame]
        sdlvl = min(sum(abs(msb16(x)) for x in frame) // config.frame, LEVEL_MAX)
        changed = False
        if state == SadState.LEARN:
            learned.append(sdlvl)
            if len(learned) == config.learn_frames:
                noise = quantize(Fraction(sum(learned), config.learn_frames))
                state = SadState.MONITOR
            out.append(SadFrame(sdlvl, math.floor(noise), state, changed))
            continue
        code = config.snthr
        if config.hysten and config.sadmod < 2 and state == SadState.DETECT:
            code = max(code - 1, 0)
        gain = GAINS[code]
        if config.sadmod == 0:
            triggered = sdlvl > noise * gain
            if not triggered:
                noise = max(update_noise(noise, sdlvl, config.anslp), Fraction(config.anmin))
        elif config.sadmod == 1:
            triggered = sdlvl > config.anmin * gain
        else:
            noise = update_noise(noise, sdlvl, config.anslp)
            triggered = noise * gain > 4 * config.anmin
        if triggered:
            changed = state == SadState.MONITOR
            state = SadState.DETECT
            hang = config.hangover
        elif state == SadState.DETECT:
            if hang == 0:
                state = SadState.MONITOR
                changed = True
            else:
                hang -= 1
        out.append(SadFrame(sdlvl, math.floor(noise), state, changed))
    return out

"""The SAD reference against RM0456 Tables 399 and 400, and its committed vectors."""

from __future__ import annotations

import json
import math
from fractions import Fraction

import pytest

from .sad_golden import VECTORS, vectors
from .sad_reference import GAINS, GAINS_DB, SadConfig, SadState, msb16, sad, update_noise

HYSTERESIS_DB = (0.0, 2.5, 3.5, 2.5, 3.5, 2.5, 3.5, 2.5, 3.5, 2.5)


def test_gains_match_table_400_in_db() -> None:
    for code, gain in enumerate(GAINS):
        assert 20 * math.log10(gain) == pytest.approx(GAINS_DB[code], abs=0.1)
        low = GAINS[max(code - 1, 0)]
        assert 20 * math.log10(gain / low) == pytest.approx(HYSTERESIS_DB[code], abs=0.1)


def slope_db(frsize: int, anslp: int, up: bool) -> float:
    """dB change of ANLVL over one second at 16 kHz, driven always up or always down."""
    config = SadConfig(
        sadmod=2, frsize=frsize, lfrnb=0, hgovr=0, anslp=anslp, snthr=0, hysten=False, anmin=0
    )
    start = Fraction(1000 if up else 10000)
    noise = start
    frames = 16000 // config.frame
    for _ in range(frames):
        noise = update_noise(noise, 0x7FFF if up else 0, anslp)
    return 20 * math.log10(noise / start) * 16000 / (frames * config.frame)


def formula_db(frsize: int, anslp: int, up: bool) -> float:
    """SLC_UP or SLC_DN of RM0456 40.4.10 at 16 kHz."""
    step = 2.0 ** (anslp - 12) if up else -(2.0 ** (anslp - 10))
    return 20 * 16000 / (8 * 2**frsize) * math.log10(1 + step)


@pytest.mark.parametrize("frsize", range(7))
def test_slopes_match_table_399_at_16_khz(frsize: int) -> None:
    for offset, up, down in ((-1, 2.1, -8.5), (0, 4.2, -17.1), (1, 8.5, -34.2)):
        anslp = frsize + offset
        if not 0 <= anslp <= 7:
            continue
        for rising, footnote in ((True, up), (False, down)):
            measured = slope_db(frsize, anslp, rising)
            assert measured == pytest.approx(formula_db(frsize, anslp, rising), rel=0.01)
            if (frsize, anslp, rising) != (6, 7, False):
                assert measured == pytest.approx(footnote, rel=0.05)


def test_fast_fall_at_512_samples_is_the_formula_not_the_footnote() -> None:
    assert formula_db(6, 7, False) == pytest.approx(-36.24, abs=0.01)
    assert slope_db(6, 7, False) == pytest.approx(-36.24, abs=0.05)


def test_msb16_takes_bits_23_to_8_signed() -> None:
    assert msb16(0x7FFFFF) == 0x7FFF
    assert msb16(-(1 << 23)) == -0x8000
    assert msb16(-1) == -1
    assert msb16(0xFF) == 0
    assert msb16(-0x100) == -1
    assert msb16(-0x101) == -2


def test_learn_then_monitor_then_detect_with_hangover() -> None:
    config = SadConfig(
        sadmod=1, frsize=0, lfrnb=0, hgovr=0, anslp=0, snthr=0, hysten=False, anmin=100
    )
    levels = [10, 10, 10, 200, 10, 10, 10, 10, 10, 10]
    pcm = [level * 256 for level in levels for _ in range(config.frame)]
    frames = sad(pcm, config)
    assert [f.sdlvl for f in frames] == levels
    assert [f.anlvl for f in frames] == [0] + [10] * 9
    assert [f.state for f in frames] == [
        SadState.LEARN,
        SadState.MONITOR,
        SadState.MONITOR,
        SadState.DETECT,
        SadState.DETECT,
        SadState.DETECT,
        SadState.DETECT,
        SadState.DETECT,
        SadState.MONITOR,
        SadState.MONITOR,
    ]
    assert [i for i, f in enumerate(frames) if f.changed] == [3, 8]


def test_full_scale_negative_frame_saturates_sdlvl() -> None:
    config = SadConfig(
        sadmod=1, frsize=0, lfrnb=0, hgovr=0, anslp=0, snthr=0, hysten=False, anmin=100
    )
    assert sad([-(1 << 23)] * config.frame, config)[0].sdlvl == 0x7FFF


def test_every_committed_sad_vector_is_generated() -> None:
    assert sorted(path.stem for path in VECTORS.glob("sad_*.json")) == sorted(vectors())


@pytest.mark.parametrize("name", sorted(vectors()))
def test_committed_vectors_are_current(name: str) -> None:
    committed = json.loads((VECTORS / f"{name}.json").read_text())
    assert committed == json.loads(json.dumps(vectors()[name]))

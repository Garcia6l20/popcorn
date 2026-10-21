"""Two PDM microphones on MDF1, interleaved into one circular GPDMA ring by the ST HAL.

The firmware starts filter 1 with `HAL_MDF_AcqStart`, filter 0 with `HAL_MDF_AcqStart_DMA` and
pulses TRGO with `HAL_MDF_GenerateTrgo`, so both start together. With ILVNB 1 the ring takes
filter 0 and filter 1 in turn, the 16 MSBs of each sample. After four complete callbacks it
stops both filters and reaches `captured`.

The tests write a 1 kHz sine to the microphone on the falling edge of SITF0 and a 3 kHz sine to
the one on the rising edge of SITF1, 16 kHz PCM at -12 dBFS, before the firmware starts.
`m.periph.MDF1.output(n)` holds every sample filter n wrote, with its time.

Every test runs strict: a write that breaks an RM0456 rule, such as the clock ratios of Table
374, raises ModelError instead of logging a warning.
"""

import cmath
import itertools
import math
import struct

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.board import FilterSample
from popcorn.units import us

pytestmark = pytest.mark.popcorn(strict=True)

RATE = 16_000
RING = 128
COMPLETES = 4
PAIRS = RING // 2 * COMPLETES
TONES = (1000, 3000)
SAMPLE_PS = 10**12 // RATE
BUDGET = 9_100
SATURATING_BUDGET = 18_100
RUN_FOR = us(21_000)
MDF_ERROR_SATURATION = 0x10


def sine(hz: int, count: int, dbfs: float) -> list[int]:
    amplitude = 32768 * 10 ** (dbfs / 20)
    return [round(amplitude * math.sin(2 * math.pi * hz * k / RATE)) for k in range(count)]


async def capture(
    machine: Machine, dbfs: float = -12, budget: int = BUDGET
) -> tuple[list[FilterSample], list[FilterSample]]:
    """Feed both microphones at `dbfs`, run to `captured` and take every sample of filters 0
    and 1.

    The run is bounded by instructions and virtual time, each a quarter above what it takes:
    7257 instructions, 14457 when every sample saturates, and 16.73 ms. The budget counts
    executed instructions only, so the time bound is what stops a firmware sleeping for an
    interrupt that never comes."""
    mdf = machine.periph.MDF1
    await mdf.mic(0, "falling").write_pcm(sine(TONES[0], 2 * PAIRS, dbfs), hz=RATE)
    await mdf.mic(1, "rising").write_pcm(sine(TONES[1], 2 * PAIRS, dbfs), hz=RATE)
    stop = await machine.run(until="captured", budget=budget, for_=RUN_FOR)
    assert stop.reason is StopReason.ADDRESS, stop
    return mdf.output(0).take(), mdf.output(1).take()


def ring(machine: Machine) -> list[int]:
    return list(struct.unpack(f"<{RING}h", machine.mem.read(machine.sym("ring"), 2 * RING)))


def dbfs(samples: list[FilterSample], hz: int) -> float:
    """Level of the `hz` component of `samples` in dB below a 24-bit full scale sine."""
    total = sum(s.pcm * cmath.exp(-2j * math.pi * hz * k / RATE) for k, s in enumerate(samples))
    return 20 * math.log10(max(2 * abs(total) / len(samples) / (1 << 23), 1e-9))


async def test_the_ring_holds_the_16_msbs_of_both_filters_in_turn_filter_0_first(
    pdm_mics: Machine,
) -> None:
    first, second = await capture(pdm_mics)
    assert (len(first), len(second)) == (PAIRS, PAIRS)
    assert not any(s.overrun for s in first + second)
    words = [s.pcm >> 8 for pair in zip(first, second, strict=True) for s in pair]
    assert ring(pdm_mics) == words[-RING:]


async def test_each_filter_hears_only_the_microphone_on_its_own_edge(pdm_mics: Machine) -> None:
    outputs = await capture(pdm_mics)
    for samples, own, other in zip(outputs, TONES, reversed(TONES), strict=True):
        settled = samples[-160:]
        assert -3.5 < dbfs(settled, own) < -2, own
        assert dbfs(settled, other) < -70, own


async def test_half_and_complete_callbacks_come_after_each_32_sample_pairs(
    pdm_mics: Machine,
) -> None:
    seen: dict[str, list[int]] = {}

    def recorder(name: str) -> None:
        def record(call: Call) -> Return:
            seen.setdefault(name, []).append(pdm_mics.time.ps)
            return call.proceed()

        pdm_mics.intercept(name, record)

    recorder("HAL_MDF_AcqHalfCpltCallback")
    recorder("HAL_MDF_AcqCpltCallback")
    first, _ = await capture(pdm_mics)
    assert {name: len(times) for name, times in seen.items()} == {
        "HAL_MDF_AcqHalfCpltCallback": COMPLETES,
        "HAL_MDF_AcqCpltCallback": COMPLETES,
    }
    assert all(
        later.at.ps - earlier.at.ps == SAMPLE_PS for earlier, later in itertools.pairwise(first)
    )
    for name, pairs in (("HAL_MDF_AcqHalfCpltCallback", 32), ("HAL_MDF_AcqCpltCallback", 64)):
        late = [time - first[pairs - 1 + 64 * k].at.ps for k, time in enumerate(seen[name])]
        assert len(set(late)) == 1, (name, late)
        assert 0 < late[0] < us(10).ps, (name, late)
    errors = pdm_mics.var("capture").member("errors")
    assert int(errors.value) == 0


async def test_the_hal_programs_lf_master_at_twice_the_output_clock(pdm_mics: Machine) -> None:
    await capture(pdm_mics)
    mdf = pdm_mics.periph.MDF1
    assert (mdf.CKGCR.PROCDIV, mdf.CKGCR.CCKDIV, mdf.CKGCR.CCK0EN, mdf.CKGCR.CKGDEN) == (4, 1, 1, 1)
    assert (mdf.SITF0CR.SITFMOD, mdf.SITF1CR.SITFMOD) == (0, 0)
    assert (mdf.BSMX0CR.BSSEL, mdf.BSMX1CR.BSSEL) == (1, 2)
    assert (mdf.GCR.ILVNB, mdf.DFLT0CR.ACQMOD, mdf.DFLT0CICR.SCALE) == (1, 2, 0x2C)


async def test_input_above_minus_9_5_dbfs_saturates_and_calls_the_error_callback(
    pdm_mics: Machine,
) -> None:
    first, _ = await capture(pdm_mics, dbfs=-6, budget=SATURATING_BUDGET)
    assert min(s.pcm for s in first) == -(1 << 23)
    state = pdm_mics.var("capture")
    assert int(state.member("errors").value) > 0
    assert int(state.member("error_codes").value) == MDF_ERROR_SATURATION

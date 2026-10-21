"""The mdf_ll fixture firmware against the reference chain: registers written by firmware in a
two-microphone driver's order, samples moved by GPDMA1 or LPDMA1 or read in an IRQ.

The tests queue the input before the entry runs. The firmware calls `starting` just before the
write that starts its filters, where the test stops, snapshots so the interfaces settle, and
counts the bits each one took. Bit j of a stream after that stop is clocked at the j-th edge of
its kind after the stop's time, 488.28125 ns apart, so a sample's `at` gives the stream index of
the bit that completed it. Expected PCM comes from `mdf_reference.run_chain` on the stream from
the filter's first bit, never from the model.

Each run is bounded by its entry's executed instructions and virtual time, both measured, plus
a quarter. The budget counts executed instructions only, so the time bound is what stops a
firmware sleeping in WFI for an interrupt that never comes.
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import math
import struct
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, StopReason, UnmodelledConfig
from popcorn.board import FilterSample
from popcorn.units import VirtualDuration, VirtualTime, us

from .conftest import run_bounded
from .mdf_reference import ChainConfig, modulate, run_chain
from .sad_reference import SadConfig, SadState, sad
from .test_pdm import BITS_PER_SAMPLE, DIVIDER, KERNEL, Edge, pack

ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "mdf_ll.elf"
PERIOD = Fraction(DIVIDER * 10**12, KERNEL)
MICS: tuple[tuple[int, Edge], ...] = ((0, "falling"), (1, "rising"))
CHAIN = ChainConfig(order=5, decimation=32, scale=0x2C, rsflt=True, hpf=2)
RING = 128
HALF = RING // 2
HALVES = 8
LOGGED = 4
SATF = 1 << 9
ROUNDING = 2
"""Edge times are floored to the picosecond, so an edge within this many ps of a stop is the
stop's own edge, which the stop's settle took."""


@dataclass(frozen=True, slots=True)
class Bound:
    """Executed instructions and virtual time of a whole run, its measured values plus a
    quarter."""

    instructions: int
    time: VirtualDuration


BOOT = Bound(2_800, us(700))
BOUNDS = {
    "two_mics": Bound(4_000, us(20_500)),
    "two_mics_lf": Bound(4_000, us(20_500)),
    "two_mics_restart": Bound(4_100, us(20_500)),
    "independent_irq": Bound(4_500, us(5_400)),
    "single_shot": Bound(7_000, us(630)),
    "manchester": Bound(3_100, us(390)),
    "adf_sad": Bound(4_500, us(38_000)),
}


def tone(hz: int, dbfs: float, count: int, rate: int = 16_000) -> list[int]:
    amplitude = 32768 * 10 ** (dbfs / 20)
    return [round(amplitude * math.sin(2 * math.pi * hz * k / rate)) for k in range(count)]


def reach(m: popcorn.Machine, symbol: str, bound: Bound) -> None:
    stop = asyncio.run(m.run(until=symbol, budget=bound.instructions, for_=bound.time))
    assert stop.reason is StopReason.ADDRESS, stop


@dataclass(frozen=True, slots=True)
class Fixture:
    """A machine at `entry` of mdf_ll, each run bounded by the entry's `Bound`."""

    m: popcorn.Machine
    bound: Bound

    def reach(self, symbol: str) -> None:
        reach(self.m, symbol, self.bound)


def fixture(u575: popcorn.PartDescription, entry: str, *, strict: bool = False) -> Fixture:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(ELF), strict_peripherals=strict)
    m.reset()
    reach(m, "main", BOOT)
    m.reg.pc = m.sym(entry)
    return Fixture(m, BOUNDS[entry])


def feed(m: popcorn.Machine, unit: str, sitf: int, edge: Edge, bits: list[int]) -> None:
    asyncio.run(m.periph[unit].mic(sitf, edge).write_pdm(pack(bits), bits=len(bits)))


@dataclass(frozen=True, slots=True)
class Start:
    """A stop at `starting`: its time and the bits each fed interface had taken by then."""

    time: VirtualTime
    taken: dict[tuple[int, Edge], int]

    def index(self, sitf: int, edge: Edge, at: VirtualTime) -> int:
        """Stream index of the bit clocked at `at`, an edge of kind `edge`."""
        span = Fraction(at.ps - self.time.ps) / PERIOD
        assert span > 0
        return self.taken[sitf, edge] + math.ceil(span - ROUNDING / PERIOD) - 1

    def first(self, sitf: int, edge: Edge, samples: list[FilterSample]) -> int:
        """Stream index of the filter's first bit, every sample's time checked against it."""
        last = [self.index(sitf, edge, s.at) for s in samples]
        start = last[0] - (BITS_PER_SAMPLE - 1)
        assert last == [start + BITS_PER_SAMPLE * (k + 1) - 1 for k in range(len(samples))]
        assert 0 <= start - self.taken[sitf, edge] < 8
        return start


def settled(m: popcorn.Machine, fed: dict[tuple[int, Edge], list[int]], unit: str) -> Start:
    m.snapshot()
    taken = {key: len(bits) - m.periph[unit].mic(*key).queued_bits for key, bits in fed.items()}
    return Start(m.time, taken)


def started(fx: Fixture, fed: dict[tuple[int, Edge], list[int]], unit: str = "MDF1") -> Start:
    fx.reach("starting")
    return settled(fx.m, fed, unit)


def u32s(m: popcorn.Machine, name: str, count: int, *, signed: bool = False) -> list[int]:
    return list(
        struct.unpack(f"<{count}{'i' if signed else 'I'}", m.mem.read(m.sym(name), 4 * count))
    )


def log(m: popcorn.Machine) -> list[int]:
    return u32s(m, "log_buf", m.mem.u32[m.sym("log_len")])


def ring(m: popcorn.Machine) -> list[int]:
    return list(struct.unpack(f"<{RING}h", m.mem.read(m.sym("ring"), 2 * RING)))


def mic_bits(dbfs: float, count: int) -> dict[tuple[int, Edge], list[int]]:
    """1 kHz on SITF0's falling edge, 3 kHz on SITF1's rising edge, 16 kHz PCM modulated."""
    return {
        key: modulate(tone(hz, dbfs, count), [BITS_PER_SAMPLE] * count)
        for key, hz in zip(MICS, (1000, 3000), strict=True)
    }


def interleaved(pcm: list[list[int]]) -> list[int]:
    """The 16-bit ring words of two interleaved filters: PCM[23:8], filter 0 first."""
    return [values[k] >> 8 for k in range(len(pcm[0])) for values in pcm]


def halves_logged(sent: list[int]) -> list[int]:
    return [word & 0xFFFF for h in range(len(sent) // HALF) for word in sent[HALF * h :][:LOGGED]]


def warnings(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [r for r in caplog.records if r.name == "popcorn" and r.levelno >= logging.WARNING]


def check_table_374(caplog: pytest.LogCaptureFixture) -> None:
    warned = warnings(caplog)
    assert len(warned) == 1, [r.getMessage() for r in warned]
    message = warned[0].getMessage()
    assert getattr(warned[0], "popcorn_peripheral", None) == "MDF1"
    for word in ("Table 374", "CKGCR PROCDIV 4", "CCKDIV 1", "SITF0CR SITFMOD 1"):
        assert word in message, message


def chain_output(
    m: popcorn.Machine, start: Start, fed: dict[tuple[int, Edge], list[int]], count: int
) -> list[list[int]]:
    """Each filter's tap PCM, checked against the reference from its first bit."""
    pcm: list[list[int]] = []
    for f, key in enumerate(MICS):
        got = m.periph.MDF1.output(f).take()
        assert len(got) == count, (f, len(got))
        assert not any(s.overrun for s in got)
        first = start.first(*key, got)
        want = run_chain(fed[key][first:], CHAIN)
        assert [s.pcm for s in got] == want.pcm[:count], f
        pcm.append(want.pcm[:count])
    return pcm


@pytest.mark.parametrize(
    ("entry", "dbfs", "table_374"),
    [("two_mics", -12, True), ("two_mics", -6, True), ("two_mics_lf", -12, False)],
    ids=["spi", "spi-saturating", "lf_master"],
)
def test_two_interleaved_mics_fill_the_16_bit_ring_with_the_reference(
    u575: popcorn.PartDescription,
    caplog: pytest.LogCaptureFixture,
    entry: str,
    dbfs: float,
    table_374: bool,
) -> None:
    """At -12 dBFS the chain stays below the -9.5 dBFS where it saturates, at -6 dBFS SATF sets
    on purpose. Normal SPI at 2 x the CCK logs the one Table 374 warning, LF_MASTER none."""
    count = HALVES * HALF // 2
    fed = mic_bits(dbfs, count + 8)
    fx = fixture(u575, entry)
    m = fx.m
    for (sitf, edge), bits in fed.items():
        feed(m, "MDF1", sitf, edge, bits)
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        start = started(fx, fed)
        fx.reach("finish")
    pcm = chain_output(m, start, fed, count)
    sent = interleaved(pcm)
    assert ring(m) == sent[-RING:]
    assert log(m) == halves_logged(sent)
    assert m.mem.u32[m.sym("halves")] == HALVES
    saturated = [any(run_chain(fed[key], CHAIN).saturated) for key in MICS]
    assert saturated == [dbfs > -9.5] * 2
    assert [bool(flags & SATF) for flags in u32s(m, "flags", 2)] == saturated
    if table_374:
        check_table_374(caplog)
    else:
        assert warnings(caplog) == []


def test_strict_peripherals_stops_the_two_mics_at_sitfen_naming_sitf0cr(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    """Open question 5: the Table 374 break is the warning by default, a ModelError under
    strict_peripherals at the first SITFEN, naming the register written. The warning before it
    names the CKGCR fields."""
    fx = fixture(u575, "two_mics", strict=True)
    m = fx.m
    with caplog.at_level(logging.WARNING, logger="popcorn"), pytest.raises(ModelError) as raised:
        fx.reach("finish")
    assert type(raised.value) is ModelError
    assert (raised.value.peripheral, raised.value.register) == ("MDF1", "SITF0CR")
    check_table_374(caplog)
    assert m.periph.MDF1.SITF0CR.SITFEN == 0


def test_a_restart_runs_a_fresh_chain_from_filter_0_with_no_new_warning(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    """Four halves, the driver's stop, its start again with the shared channel enabled twice,
    four more halves. Each run equals the reference from its own first bit."""
    count = HALVES * HALF // 4
    fed = mic_bits(-12, 2 * count + 16)
    fx = fixture(u575, "two_mics_restart")
    m = fx.m
    for (sitf, edge), bits in fed.items():
        feed(m, "MDF1", sitf, edge, bits)
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        first = started(fx, fed)
        fx.reach("starting")
        assert m.mem.u32[m.sym("halves")] == HALVES // 2
        before = chain_output(m, first, fed, count)
        second = settled(m, fed, "MDF1")
        fx.reach("finish")
    after = chain_output(m, second, fed, count)
    assert [second.taken[key] - first.taken[key] for key in MICS] >= [count * BITS_PER_SAMPLE] * 2
    sent = interleaved(after)
    assert ring(m) == sent[-RING:]
    assert log(m) == halves_logged(interleaved(before)) + halves_logged(sent)
    check_table_374(caplog)


def test_an_independent_filter_irq_reads_two_samples_per_rxfifo_threshold(
    u575: popcorn.PartDescription,
) -> None:
    count = 64
    fed: dict[tuple[int, Edge], list[int]] = {
        (0, "rising"): modulate(tone(1000, -12, count + 8), [BITS_PER_SAMPLE] * (count + 8))
    }
    fx = fixture(u575, "independent_irq")
    m = fx.m
    feed(m, "MDF1", 0, "rising", fed[0, "rising"])
    start = started(fx, fed)
    fx.reach("finish")
    got = m.periph.MDF1.output(0).take()[:count]
    first = start.first(0, "rising", got)
    want = run_chain(fed[0, "rising"][first:], CHAIN).pcm[:count]
    assert [s.pcm for s in got] == want
    assert not any(s.overrun for s in got)
    assert [value >> 8 for value in u32s(m, "samples", count, signed=True)] == want
    assert m.mem.u32[m.sym("sample_irqs")] == count // 2


def test_sync_single_shot_gives_one_sample_per_trgo(u575: popcorn.PartDescription) -> None:
    """Each TRGO restarts the chain from its reset state at the next edge, so each sample is the
    reference of the 128 bits before its own time."""
    shots = 3
    bits = modulate(tone(1000, -12, 40), [BITS_PER_SAMPLE] * 40)
    fed: dict[tuple[int, Edge], list[int]] = {(0, "rising"): bits}
    fx = fixture(u575, "single_shot")
    m = fx.m
    feed(m, "MDF1", 0, "rising", bits)
    start = started(fx, fed)
    fx.reach("finish")
    got = m.periph.MDF1.output(0).take()
    assert len(got) == shots
    ends = [start.index(0, "rising", s.at) for s in got]
    assert all(b - a >= BITS_PER_SAMPLE for a, b in itertools.pairwise(ends))
    want = [run_chain(bits[end - BITS_PER_SAMPLE + 1 : end + 1], CHAIN).pcm[0] for end in ends]
    assert [s.pcm for s in got] == want
    assert [value >> 8 for value in u32s(m, "samples", shots, signed=True)] == want
    assert u32s(m, "shot_runs", shots) == [0] * shots


def test_manchester_at_sitfen_raises_unmodelled_config_and_keeps_sitf0cr(
    u575: popcorn.PartDescription,
) -> None:
    fx = fixture(u575, "manchester")
    m = fx.m
    with pytest.raises(UnmodelledConfig) as raised:
        fx.reach("finish")
    error = raised.value
    assert (error.peripheral, error.register, error.field, error.value) == (
        "MDF1",
        "SITF0CR",
        "SITFMOD",
        2,
    )
    assert (m.periph.MDF1.SITF0CR.SITFEN, m.periph.MDF1.SITF0CR.SITFMOD) == (0, 2)


ADF_CHAIN = ChainConfig(order=5, decimation=16, rsflt=True, hpf=3)
SAD = SadConfig(sadmod=0, frsize=2, lfrnb=1, hgovr=0, anslp=3, snthr=3, hysten=False, anmin=20)
SAD_FRAMES = 30
SAD_RING = 64
SRAM4 = 0x2800_0000


def test_the_adf_detector_lets_lpdma1_fill_its_ring_only_in_detect(
    u575: popcorn.PartDescription,
) -> None:
    """Silence, a 1 kHz burst of 10 frames, silence, at 32 kHz. The SAD frames, levels and
    DATCAP 01 come from `sad_reference.sad` on the reference chain's output."""
    frame = SAD.frame
    quiet, loud = 8, 10
    total = (SAD_FRAMES + 2) * frame
    pcm16 = [
        value if quiet * frame <= k < (quiet + loud) * frame else 0
        for k, value in enumerate(tone(1000, -12, total, rate=32_000))
    ]
    bits = modulate(pcm16, [64] * total)
    fed: dict[tuple[int, Edge], list[int]] = {(0, "rising"): bits}
    fx = fixture(u575, "adf_sad")
    m = fx.m
    feed(m, "ADF1", 0, "rising", bits)
    start = started(fx, fed, "ADF1")
    fx.reach("finish")
    samples = SAD_FRAMES * frame
    got = m.periph.ADF1.output(0).take()
    assert len(got) == samples
    last = [start.index(0, "rising", s.at) for s in got]
    first = last[0] - 63
    assert last == [first + 64 * (k + 1) - 1 for k in range(samples)]
    pcm = run_chain(bits[first:], ADF_CHAIN).pcm[:samples]
    assert [s.pcm for s in got] == pcm
    frames = sad(pcm, SAD)
    states = [SadState.LEARN] + [f.state for f in frames]
    captured = [states[k // frame] == SadState.DETECT for k in range(samples)]
    assert [s.captured for s in got] == captured
    entered = [
        k + 1
        for k, f in enumerate(frames)
        if f.state == SadState.DETECT and states[k] != SadState.DETECT
    ]
    assert len(entered) == 1
    assert m.mem.u32[m.sym("sad_detections")] == len(entered)
    assert u32s(m, "sad_detected", 1) == entered
    assert u32s(m, "sad_levels", SAD_FRAMES) == [f.sdlvl for f in frames]
    assert m.mem.u32[m.sym("sad_state")] >> 4 == int(frames[-1].state)
    kept = [value << 8 & 0xFFFF_FFFF for value, keep in zip(pcm, captured, strict=True) if keep]
    assert len(kept) > SAD_RING
    want = [kept[max(i for i in range(len(kept)) if i % SAD_RING == p)] for p in range(SAD_RING)]
    assert list(struct.unpack(f"<{SAD_RING}I", m.mem.read(SRAM4, 4 * SAD_RING))) == want


@dataclass(frozen=True, slots=True)
class Outcome:
    ring: list[int]
    log: list[int]
    flags: list[int]
    taps: list[list[FilterSample]]


def outcome(m: popcorn.Machine) -> Outcome:
    return Outcome(
        ring(m), log(m), u32s(m, "flags", 2), [m.periph.MDF1.output(f).take() for f in range(2)]
    )


def test_two_mics_give_the_same_ring_samples_and_times_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    fed = mic_bits(-12, HALVES * HALF // 2 + 8)
    legs: list[Outcome] = []
    for running in (False, True):
        fx = fixture(u575, "two_mics")
        m = fx.m
        for (sitf, edge), bits in fed.items():
            feed(m, "MDF1", sitf, edge, bits)
        if running:

            async def body(m: popcorn.Machine = m, bound: Bound = fx.bound) -> popcorn.Stop:
                async with m.running():
                    return await m.wait_for(m.ref.var("done").ge(1), for_=bound.time)

            assert run_bounded(body(), 30).reason is StopReason.USER
            assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]
        else:
            fx.reach("finish")
        legs.append(outcome(m))
    assert legs[0] == legs[1]
    assert [len(taps) for taps in legs[0].taps] == [HALVES * HALF // 2] * 2

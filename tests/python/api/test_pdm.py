"""PDM microphones and digital filter outputs of the MDF and ADF, from Python, firmware-free.

The rcc_ll firmware idles while the tests write the registers. PLL3 Q gives the kernel 20.48
MHz, PROCDIV 4 and CCKDIV 1 the peer's CCK0 of 2.048 MHz: one bit per edge kind every 488.28125
ns. The CCK starts at the CKGDEN write, the anchor, and half edge h falls at
`anchor + floor(h x 10 x 10^12 / (2 x 20.48 MHz))`, rising at even h."""

from __future__ import annotations

import asyncio
import gc
import json
import math
import weakref
from pathlib import Path
from typing import Literal

import popcorn
import pytest
from popcorn import ResetKind, System, UartLink
from popcorn.board import FilterSample, FilterTap, PdmMic
from popcorn.units import VirtualTime, ms, us

from .conftest import run_bounded
from .mdf_reference import ChainConfig, modulate, run_chain

ROOT = Path(__file__).parents[3]
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
VECTOR = ROOT / "tests" / "data" / "periph" / "mdf" / "chain_pdm_16k.json"
KERNEL = 20_480_000
DIVIDER = 10
PEER = ChainConfig(order=5, decimation=32, scale=0x2C, rsflt=True, hpf=2)
BITS_PER_SAMPLE = 128

type Edge = Literal["rising", "falling"]


def pdm_16k() -> tuple[bytes, int, list[int]]:
    """The raw input of `chain_pdm_16k.json`, its bit count and its PCM."""
    run = json.loads(VECTOR.read_text())["runs"][0]
    pcm = [value for value, count in run["pcm"] for _ in range(count)]
    return bytes.fromhex(run["bits"]["hex"]), run["bits"]["count"], pcm


def bits_of(data: bytes, count: int) -> list[int]:
    return [(data[k // 8] >> (7 - k % 8)) & 1 for k in range(count)]


def edge_at(anchor: VirtualTime, h: int) -> VirtualTime:
    return VirtualTime(anchor.ps + h * DIVIDER * 10**12 // (2 * KERNEL))


def edges_by(anchor: VirtualTime, t: VirtualTime, edge: Edge = "rising") -> int:
    """Edges of one kind after the anchor and at or before `t`."""
    first = 2 if edge == "rising" else 1
    count = 0
    while edge_at(anchor, first + 2 * count) <= t:
        count += 1
    return count


def sample_at(anchor: VirtualTime, k: int) -> VirtualTime:
    """Time of the peer chain's sample `k` on rising edges from the anchor."""
    return edge_at(anchor, 2 + 2 * (BITS_PER_SAMPLE * (k + 1) - 1))


def clocked(m: popcorn.Machine) -> None:
    """PLL3 Q at 20.48 MHz as MDF1 and ADF1 kernel, pc at the firmware's idle loop."""
    m.reg.pc = m.sym("idle")
    rcc = m.periph.RCC
    rcc.RCC_AHB1ENR.MDF1EN = 1
    rcc.RCC_AHB3ENR.ADF1EN = 1
    rcc.RCC_CR.HSION = 1
    asyncio.run(m.run(for_=us(200)))
    rcc.RCC_PLL3CFGR.write((2 << 0) | (3 << 2) | (4 << 8) | (1 << 17))
    rcc.RCC_PLL3DIVR.write(63 | (9 << 16))
    rcc.RCC_CR.PLL3ON = 1
    asyncio.run(m.run(for_=us(200)))
    assert rcc.RCC_CR.PLL3RDY == 1
    rcc.RCC_CCIPR2.MDF1SEL = 2
    rcc.RCC_CCIPR3.ADF1SEL = 2


def machine(u575: popcorn.PartDescription, name: str | None = None) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), name=name)
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is popcorn.StopReason.ADDRESS
    clocked(m)
    return m


def cck(m: popcorn.Machine, unit: str = "MDF1") -> VirtualTime:
    """Starts CCK0 at 2.048 MHz, returns the anchor."""
    view = m.periph[unit]
    view.CKGCR.write((4 << 24) | (1 << 16) | (1 << 5) | (1 << 1))
    view.CKGCR.CKGDEN = 1
    return m.time


def sitf(m: popcorn.Machine, index: int = 0, unit: str = "MDF1") -> None:
    m.periph[unit][f"SITF{index}CR"].write(0x1F11)


def peer_filter(m: popcorn.Machine, f: int = 0, stream: int = 0, unit: str = "MDF1") -> None:
    """The peer's chain, Sinc5 D 32, SCALE 0x2C, RSFLT, HPF 2, on bitstream `stream`."""
    view = m.periph[unit]
    view[f"BSMX{f}CR"].write(stream)
    view[f"DFLT{f}CICR"].write((0x2C << 20) | (31 << 8) | (5 << 4))
    view[f"DFLT{f}RSFR"].write(2 << 8)
    view[f"DFLT{f}CR"].write(1)


def sine(dbfs: float, count: int, hz: int = 48_000) -> list[int]:
    amplitude = 32768 * 10 ** (dbfs / 20)
    return [round(amplitude * math.sin(2 * math.pi * 1000 * i / hz)) for i in range(count)]


def held_bits(count: int, hz: int) -> list[int]:
    """Bits each of `count` samples at `hz` is held for, from `floor(k x hz x DIVIDER /
    KERNEL)`."""

    def start(i: int) -> int:
        return -(-i * KERNEL // (hz * DIVIDER))

    return [start(i + 1) - start(i) for i in range(count)]


def read(tap: FilterTap, n: int) -> list[FilterSample]:
    samples = asyncio.run(tap.read(n, for_=ms(100)))
    assert len(samples) == n
    return samples


def test_raw_bits_through_the_peer_chain_give_the_vector_at_the_closed_form_times(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    data, count, pcm = pdm_16k()
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    asyncio.run(mic.write_pdm(data, bits=count))
    assert (mic.queued_bits, mic.queued_samples, mic.idle_bits) == (count, 0, 0)
    anchor = cck(m)
    sitf(m)
    peer_filter(m)
    got = read(tap, len(pcm))
    assert [s.pcm for s in got] == pcm
    assert [s.at for s in got] == [sample_at(anchor, k) for k in range(len(pcm))]
    assert all(s.captured for s in got)
    assert [s.overrun for s in got] == [False] * 4 + [True] * (len(pcm) - 4)
    assert (mic.queued_bits, tap.available, tap.dropped) == (0, 0, 0)


@pytest.mark.parametrize(("dbfs", "saturates"), [(-12, False), (-6, True)])
def test_pcm_through_the_modulator_and_the_peer_chain_equals_the_reference(
    u575: popcorn.PartDescription, dbfs: float, saturates: bool
) -> None:
    m = machine(u575)
    pcm = sine(dbfs, 480)
    held = held_bits(len(pcm), 48_000)
    assert sum(held) == 160 * BITS_PER_SAMPLE
    mic = m.periph.MDF1.mic(0, "rising")
    asyncio.run(mic.write_pcm(pcm, hz=48_000))
    assert mic.queued_samples == len(pcm)
    cck(m)
    sitf(m)
    peer_filter(m)
    got = read(m.periph.MDF1.output(0), 160)
    want = run_chain(modulate(pcm, held), PEER)
    assert [s.pcm for s in got] == want.pcm
    assert any(want.saturated) is saturates
    assert int(saturates) == m.periph.MDF1.DFLT0ISR.SATF
    assert (mic.queued_samples, mic.idle_bits) == (0, 0)


def test_a_write_takes_the_edges_after_its_time_and_silence_fills_those_before(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    data, count, _ = pdm_16k()
    bits = bits_of(data, count)
    mic = m.periph.MDF1.mic(0, "rising")
    anchor = cck(m)
    sitf(m)
    asyncio.run(m.run(for_=us(100)))
    t1 = asyncio.run(mic.write_pdm(data, bits=count))
    idle = edges_by(anchor, t1)
    assert mic.idle_bits == idle
    asyncio.run(m.run(for_=us(73)))
    t2 = m.time
    peer_filter(m)
    skipped = edges_by(anchor, t2) - idle
    got = read(m.periph.MDF1.output(0), 64)
    assert [s.pcm for s in got] == run_chain(bits[skipped:], PEER).pcm[:64]
    assert mic.queued_bits == count - skipped - 64 * BITS_PER_SAMPLE


def test_a_write_while_the_filter_runs_lands_at_the_first_edge_after_its_time(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    data, count, _ = pdm_16k()
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    anchor = cck(m)
    sitf(m)
    peer_filter(m)
    asyncio.run(m.run(for_=us(333)))
    at = asyncio.run(mic.write_pdm(data, bits=count))
    idle = edges_by(anchor, at)
    total = (idle + count) // BITS_PER_SAMPLE
    got = read(tap, total)
    stream = modulate([0], [idle]) + bits_of(data, count)
    assert [s.pcm for s in got] == run_chain(stream, PEER).pcm[:total]
    assert [s.at for s in got] == [sample_at(anchor, k) for k in range(total)]
    assert (mic.idle_bits, mic.queued_bits) == (idle, idle + count - total * BITS_PER_SAMPLE)


def test_both_edges_of_one_line_feed_their_own_filters(u575: popcorn.PartDescription) -> None:
    m = machine(u575)
    data, count, pcm = pdm_16k()
    rising = m.periph.MDF1.mic(0, "rising")
    falling = m.periph.MDF1.mic(0, "falling")
    asyncio.run(falling.write_pdm(data, bits=count))
    asyncio.run(rising.write_pcm([0] * 64, hz=16_000))
    anchor = cck(m)
    sitf(m)
    peer_filter(m, 0, 1)
    peer_filter(m, 1, 0)
    got = read(m.periph.MDF1.output(0), 64)
    assert [s.pcm for s in got] == pcm[:64]
    assert got[0].at == edge_at(anchor, 1 + 2 * (BITS_PER_SAMPLE - 1))
    silent = read(m.periph.MDF1.output(1), 64)
    assert [s.pcm for s in silent] == run_chain(modulate([0], [64 * 128]), PEER).pcm
    assert rising.queued_samples == 0


def test_a_restore_drops_queued_input_unread_samples_and_the_modulator(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    start = m.snapshot()
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    pcm = sine(-12, 480)

    def capture() -> list[FilterSample]:
        asyncio.run(mic.write_pcm(pcm, hz=48_000))
        cck(m)
        sitf(m)
        peer_filter(m)
        return read(tap, 100)

    first = capture()
    asyncio.run(mic.write_pcm(pcm, hz=48_000))
    asyncio.run(m.run(for_=ms(1)))
    assert (mic.queued_samples > 0, tap.available > 0) == (True, True)
    tap._link.set_limit(1)  # pyright: ignore[reportPrivateUsage]
    asyncio.run(m.run(for_=ms(1)))
    assert tap.dropped > 0
    tap._link.set_limit(1 << 18)  # pyright: ignore[reportPrivateUsage]
    m.restore(start)
    assert (mic.queued_samples, mic.queued_bits, mic.idle_bits) == (0, 0, 0)
    assert (tap.available, tap.dropped) == (0, 0)
    assert capture() == first


@pytest.mark.parametrize("kind", list(ResetKind), ids=lambda kind: kind.name)
def test_a_reset_of_any_kind_keeps_queued_input_and_kept_samples(
    u575: popcorn.PartDescription, kind: ResetKind
) -> None:
    m = machine(u575)
    data, count, pcm = pdm_16k()
    mic = m.periph.MDF1.mic(0, "rising")
    kept = m.periph.MDF1.output(1)
    asyncio.run(mic.write_pdm(data, bits=count))
    cck(m)
    sitf(m, 1)
    peer_filter(m, 1, 2)
    asyncio.run(m.run(for_=ms(1)))
    before = kept.available
    assert before > 10
    m.reset(ResetKind.SOFTWARE)
    m.reg.pc = m.sym("idle")
    asyncio.run(m.run(for_=us(50)))
    m.reset(kind)
    assert (mic.queued_bits, kept.available) == (count, before)
    clocked(m)
    anchor = cck(m)
    assert anchor < VirtualTime(0) + ms(1)
    sitf(m)
    peer_filter(m)
    got = read(m.periph.MDF1.output(0), 32)
    assert [s.pcm for s in got] == pcm[:32]
    assert [s.at for s in got] == [sample_at(anchor, k) for k in range(32)]


def test_a_full_tap_drops_its_oldest_samples_and_cycles_keep_every_queue_bounded(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    anchor = cck(m)
    sitf(m)
    peer_filter(m)
    tap._link.set_limit(8)  # pyright: ignore[reportPrivateUsage]
    asyncio.run(m.run(for_=ms(1) + us(260)))
    assert (tap.available, tap.dropped) == (8, 12)
    assert [s.at for s in tap.take()] == [sample_at(anchor, k) for k in range(12, 20)]
    for _ in range(1000):
        asyncio.run(mic.write_pcm([100, -100, 50], hz=48_000))
        assert len(read(tap, 1)) == 1
        assert (tap.available, mic.queued_bits) == (0, 0)
        assert mic.queued_samples <= 3
    assert tap.dropped == 12


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_parked_and_running_reads_give_the_same_samples_and_times(
    u575: popcorn.PartDescription, running: bool
) -> None:
    m = machine(u575)
    data, count, pcm = pdm_16k()
    mic = m.periph.MDF1.mic(0, "falling")
    tap = m.periph.MDF1.output(2)
    asyncio.run(mic.write_pdm(data, bits=count))
    anchor = cck(m)
    sitf(m)
    peer_filter(m, 2, 1)

    def when(k: int) -> VirtualTime:
        return edge_at(anchor, 1 + 2 * (BITS_PER_SAMPLE * (k + 1) - 1))

    async def scenario() -> list[FilterSample]:
        first = await tap.read(100, deadline=anchor + ms(3))
        assert len(first) == sum(when(k) <= anchor + ms(3) for k in range(100))
        return first + await tap.read(500 - len(first))

    if running:

        async def free() -> list[FilterSample]:
            async with m.running():
                return await scenario()

        got = run_bounded(free(), 20)
    else:
        got = asyncio.run(scenario())
    assert [s.pcm for s in got] == pcm[:500]
    assert [s.at for s in got] == [when(k) for k in range(500)]
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def dropped_with_a_cancelled_read(u575: popcorn.PartDescription) -> weakref.ref[popcorn.Machine]:
    m = machine(u575)
    tap = m.periph.MDF1.output(0)

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(TimeoutError):
                await asyncio.wait_for(tap.read(10), 0.05)

    run_bounded(scenario(), 10)
    return weakref.ref(m)


def test_a_cancelled_read_keeps_no_machine_alive(u575: popcorn.PartDescription) -> None:
    gone = dropped_with_a_cancelled_read(u575)
    gc.collect()
    assert gone() is None


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_bad_arguments_raise_at_the_call(u575: popcorn.PartDescription, running: bool) -> None:
    m = machine(u575)
    mdf = m.periph.MDF1
    mic = mdf.mic(0, "rising")

    def refused() -> None:
        with pytest.raises(ValueError, match="-32768 to 32767"):
            mic.write_pcm([0, 32768], hz=16_000)
        with pytest.raises(TypeError, match="a PCM sample is an int"):
            mic.write_pcm([True], hz=16_000)
        with pytest.raises(ValueError, match="sample rate"):
            mic.write_pcm([0], hz=0)
        with pytest.raises(ValueError, match="sample rate"):
            mic.write_pcm([0], hz=1 << 32)
        with pytest.raises(TypeError, match="hz is an int"):
            mic.write_pcm([0], hz=True)
        with pytest.raises(ValueError, match="bits is 0 to 8"):
            mic.write_pdm(b"\x01", bits=9)
        with pytest.raises(TypeError, match="bits is an int"):
            mic.write_pdm(b"\x01", bits=False)
        with pytest.raises(popcorn.ConfigError, match="from 0 to 5"):
            mdf.mic(6, "rising")
        with pytest.raises(popcorn.ConfigError, match="rising or falling"):
            mdf.mic(0, "both")  # pyright: ignore[reportArgumentType]
        with pytest.raises(popcorn.ConfigError, match="from 0 to 5"):
            mdf.output(-1)
        with pytest.raises(popcorn.ConfigError, match="from 0 to 0"):
            m.periph.ADF1.output(1)
        with pytest.raises(popcorn.ModelError, match="no built-in MDF or ADF"):
            m.periph.USART1.mic(0, "rising")
        assert (mic.queued_samples, mic.queued_bits) == (0, 0)

    if running:

        async def scenario() -> None:
            async with m.running():
                refused()

        run_bounded(scenario(), 10)
    else:
        refused()


def test_the_secure_alias_and_the_refs_reach_the_same_sensor_and_tap(
    u575: popcorn.PartDescription,
) -> None:
    m = machine(u575)
    mic = m.periph.MDF1.mic(3, "falling")
    assert m.periph.SEC_MDF1.mic(3, "falling") is mic
    assert m.periph.SEC_MDF1.output(3) is m.periph.MDF1.output(3)
    assert isinstance(mic, PdmMic)
    asyncio.run(mic.write_pcm([1, 2, 3], hz=8000))
    asyncio.run(mic.write_pdm(b"\xff", bits=5))
    refs = m.ref.periph.MDF1.mic(3, "falling")
    tap = m.ref.periph.MDF1.output(3)

    async def values() -> tuple[object, ...]:
        return tuple(
            await m.read(refs.queued_samples, refs.queued_bits, refs.idle_bits, tap.available)
        )

    assert asyncio.run(values()) == (3, 5, 0, 0)
    assert refs.queued_bits.get() == 5
    assert not refs.queued_bits.writable


def test_the_adf_tap_marks_samples_its_detector_kept_out(u575: popcorn.PartDescription) -> None:
    m = machine(u575)
    data, count, pcm = pdm_16k()
    adf = m.periph.ADF1
    mic = adf.mic(0, "rising")
    tap = adf.output(0)
    asyncio.run(mic.write_pdm(data, bits=count))
    anchor = cck(m, "ADF1")
    sitf(m, 0, "ADF1")
    peer_filter(m, 0, 0, "ADF1")
    got = read(tap, 64)
    assert [s.pcm for s in got] == pcm[:64]
    assert [s.at for s in got] == [sample_at(anchor, k) for k in range(64)]
    assert all(s.captured for s in got)
    adf.DFLT0CR.write(0)
    adf.SADCFGR.write(100 << 16)
    adf.SADCR.write(1 << 12)
    adf.SADCR.write((1 << 12) | 1)
    peer_filter(m, 0, 0, "ADF1")
    kept_out = read(tap, 16)
    assert not any(s.captured or s.overrun for s in kept_out)
    assert adf.DFLT0ISR.RXNEF == 0


def test_a_system_member_feeds_and_taps_its_mdf_and_its_restore_drops_them(
    u575: popcorn.PartDescription,
) -> None:
    a = machine(u575, "a")
    b = machine(u575, "b")
    system = System(
        {"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=us(10), baud=460_800)]
    )
    try:
        start = system.snapshot()
        data, count, pcm = pdm_16k()
        mic = a.periph.MDF1.mic(0, "rising")
        tap = a.periph.MDF1.output(0)
        asyncio.run(mic.write_pdm(data, bits=count))
        cck(a)
        sitf(a)
        peer_filter(a)
        asyncio.run(system.run(for_=ms(33)))
        assert [s.pcm for s in tap.take()][: len(pcm)] == pcm
        asyncio.run(mic.write_pdm(data, bits=count))
        asyncio.run(system.run(for_=ms(1)))
        assert tap.available > 0
        system.restore(start)
        assert (mic.queued_bits, mic.idle_bits, tap.available) == (0, 0, 0)
    finally:
        system.close()

"""The MDF and ADF chains end to end against the reference: PDM written from Python, registers
and DMA channels programmed from Python, the RXFIFO drained by GPDMA1 or LPDMA1 into RAM, RAM
read back from Python.

The rcc_ll firmware idles while the tests write the registers. The kernel is 20.48 MHz and
CCK0 2.048 MHz, as in `test_pdm`: bit j of a stream is clocked at half edge `2 + 2 j` after the
CCK anchor on the rising edge and `1 + 2 j` on the falling edge. Expected PCM comes from the
`chain_*.json` vectors or from `mdf_reference.run_chain`, never from the model.
"""

from __future__ import annotations

import asyncio
import json
import math
import struct
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Literal, cast

import popcorn
import pytest
from popcorn import ResetKind, StopReason, System, UartLink
from popcorn.board import FilterSample
from popcorn.units import VirtualTime, us

from . import sad_golden
from .conftest import run_bounded
from .mdf_golden import BIT_RATE, Json, decode_config, expand, prbs7, tone, unpack
from .mdf_reference import ChainConfig, modulate, run_chain
from .mdf_state import MdfState, decode, encode
from .sad_reference import SadConfig, SadState, sad
from .snapshot_doc import bson
from .test_pdm import bits_of, cck, clocked, edge_at, held_bits, machine, pack, pdm_16k

DATA = Path(__file__).parents[2] / "data" / "periph" / "mdf"
MDF1 = 0x4002_5000
ADF1 = 0x4602_4000
BUFFER = 0x2004_0000
NODES = 0x200B_0000
SRAM4 = 0x2800_0000
MDF1_REQUEST = 92
ADF1_REQUEST = 10
DFLTEN = 1
DMAEN = 1 << 1
SYNC_CONTINUOUS = 2 << 4
PEER = ChainConfig(order=5, decimation=32, scale=0x2C, rsflt=True, hpf=2)
INT_CODES = {128: 0, 32: 1, 4: 2, 1: 3}
LOWER_EDGE: dict[str, int] = {"rising": 2, "falling": 1}

type Edge = Literal["rising", "falling"]


def dr(base: int, f: int) -> int:
    """Address of DFLTfDR."""
    return base + 0xF0 + 0x80 * f


def edge_of(f: int) -> Edge:
    """The edge filter `f` of the six-filter cases takes, falling on odd filters."""
    return "falling" if f % 2 else "rising"


def golden_machine(u575: popcorn.PartDescription, name: str | None = None) -> popcorn.Machine:
    """`test_pdm.machine` with HSI16 as SYSCLK, so hclk is at least mdf_proc_ck, RM0456 Table
    374, and GPDMA1 keeps up with a decimation of 2."""
    m = machine(u575, name)
    fast_bus(m)
    return m


def fast_bus(m: popcorn.Machine) -> None:
    m.periph.RCC.RCC_CFGR1.SW = 1
    asyncio.run(m.run(for_=us(100)))
    assert m.periph.RCC.RCC_CFGR1.SWS == 1


def clock_dma(m: popcorn.Machine) -> None:
    m.periph.RCC.RCC_AHB1ENR.GPDMA1EN = 1
    m.periph.RCC.RCC_AHB3ENR.LPDMA1EN = 1


def dma(
    m: popcorn.Machine,
    controller: str,
    channel: int,
    request: int,
    source: int,
    buffer: int,
    words: int,
    *,
    half: bool = False,
    ring: bool = False,
) -> None:
    """Channel `channel` moves `words` words of `source` on `request` into `buffer`, as half
    words keeping the MSBs with `half`, PAM 1. Blocks above 16383 words chain through
    linked-list nodes, a `ring` reloads its one block for ever."""
    view = m.periph[controller]
    width = 1 if half else 2
    view[f"C{channel}TR1"].write(2 | (width << 16) | (1 << 19) | ((1 << 11) if half else 0))
    view[f"C{channel}TR2"].write(request)
    step = 2 if half else 4
    blocks = [(first, min(16383, words - first)) for first in range(0, words, 16383)]
    nodes = NODES + 0x100 * channel
    reload = (1 << 29) | (1 << 27) | (1 << 16)
    if ring:
        m.mem.write(nodes, struct.pack("<3I", 4 * words, buffer, reload | (nodes & 0xFFFC)))
    for index, (first, count) in enumerate(blocks[1:]):
        last = index + 2 == len(blocks)
        follow = 0 if last else reload | ((nodes + 12 * (index + 1)) & 0xFFFC)
        node = (4 * count, buffer + step * first, follow)
        m.mem.write(nodes + 12 * index, struct.pack("<3I", *node))
    view[f"C{channel}BR1"].write(4 * blocks[0][1])
    view[f"C{channel}SAR"].write(source)
    view[f"C{channel}DAR"].write(buffer)
    if ring or len(blocks) > 1:
        view[f"C{channel}LBAR"].write(nodes & 0xFFFF_0000)
        view[f"C{channel}LLR"].write(reload | (nodes & 0xFFFC))
    view[f"C{channel}CR"].write(1)


def words(m: popcorn.Machine, address: int, count: int) -> list[int]:
    return list(struct.unpack(f"<{count}i", m.mem.read(address, 4 * count)))


def halves(m: popcorn.Machine, address: int, count: int) -> list[int]:
    return list(struct.unpack(f"<{count}h", m.mem.read(address, 2 * count)))


def sitf(m: popcorn.Machine, index: int, unit: str = "MDF1") -> None:
    """SITF `index` in normal SPI on CCK0, STH 0x1F."""
    m.periph[unit][f"SITF{index}CR"].write(0x1F11)


def program(
    m: popcorn.Machine, f: int, config: ChainConfig, *, stream: int, cr: int, unit: str = "MDF1"
) -> None:
    """Writes `config` to filter `f` on bitstream `stream`, then DFLTxCR with `cr`."""
    view = m.periph[unit]
    order = 0 if config.order == "fastsinc" else config.order
    view[f"BSMX{f}CR"].write(stream)
    view[f"DFLT{f}CICR"].write((config.scale << 20) | ((config.decimation - 1) << 8) | (order << 4))
    view[f"DFLT{f}RSFR"].write(
        (0 if config.rsflt else 1)
        | (0 if config.rsflt_decimate else 1 << 4)
        | (1 << 7 if config.hpf is None else config.hpf << 8)
    )
    if config.int_value is not None:
        view[f"DFLT{f}INTR"].write(((config.int_value - 1) << 4) | INT_CODES[config.int_div])
    if config.offset:
        view[f"OEC{f}CR"].write(config.offset & 0x3FF_FFFF)
    if config.skip:
        view[f"DLY{f}CR"].write(config.skip)
    view[f"DFLT{f}CR"].write((config.discard << 20) | cr)


def bits_per_sample(config: ChainConfig) -> int:
    return config.decimation * (4 if config.rsflt and config.rsflt_decimate else 1)


def sample_time(
    anchor: VirtualTime, config: ChainConfig, k: int, edge: Edge = "rising"
) -> VirtualTime:
    """Edge of the bit that completes output `k` of `config`, closed form."""
    summed = config.int_value or 1
    last = config.skip + bits_per_sample(config) * (config.discard + summed * (k + 1)) - 1
    return edge_at(anchor, LOWER_EDGE[edge] + 2 * last)


def run_to(m: popcorn.Machine, t: VirtualTime) -> None:
    asyncio.run(m.run(for_=t - m.time))


def vector_pcm(run: Json) -> list[int]:
    """The expected PCM of a chain vector's run."""
    return [int(v) for v in expand([(int(v), int(n)) for v, n in run["pcm"]])]


def chain_runs() -> list[tuple[str, int]]:
    return [
        (path.stem, index)
        for path in sorted(DATA.glob("chain_*.json"))
        for index in range(len(json.loads(path.read_text())["runs"]))
    ]


@pytest.mark.parametrize(("name", "index"), chain_runs(), ids=lambda v: str(v))
def test_every_chain_vector_through_mdf1_filter_0_and_gpdma1_equals_its_pcm(
    u575: popcorn.PartDescription, name: str, index: int
) -> None:
    run = json.loads((DATA / f"{name}.json").read_text())["runs"][index]
    config = decode_config(run["config"])
    bits = unpack(run["bits"])
    pcm = vector_pcm(run)
    saturated = [bool(v) for v in expand([(int(v), int(n)) for v, n in run["saturated"]])]
    m = golden_machine(u575)
    clock_dma(m)
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    asyncio.run(mic.write_pdm(pack(bits), bits=len(bits)))
    dma(m, "GPDMA1", 0, MDF1_REQUEST, dr(MDF1, 0), BUFFER, len(pcm))
    anchor = cck(m)
    sitf(m, 0)
    program(m, 0, config, stream=0, cr=DMAEN | DFLTEN)
    times = [sample_time(anchor, config, k) for k in range(len(pcm))]
    run_to(m, times[-1] + us(10))
    got = tap.take()[: len(pcm)]
    assert [s.pcm for s in got] == pcm
    assert [s.at for s in got] == times
    assert not any(s.overrun for s in got)
    assert words(m, BUFFER, len(pcm)) == [value << 8 for value in pcm]
    assert int(any(saturated)) == m.periph.MDF1.DFLT0ISR.SATF
    assert (m.periph.GPDMA1.C0BR1.BNDT, m.periph.GPDMA1.C0SR.TCF) == (0, 1)


ORDERS: list[ChainConfig] = [
    ChainConfig(order=order, decimation=16) for order in ("fastsinc", 1, 2, 3, 4, 5)
]
SCALES: list[ChainConfig] = [
    ChainConfig(order=4, decimation=8, scale=code) for code in [*range(0x19), *range(0x20, 0x30)]
]
SWEEP = ORDERS + SCALES


def sweep_bits(f: int) -> list[int]:
    """Input of filter `f`: 4096 bits of PRBS7 from a phase of its own."""
    return prbs7(4096 + 61 * f)[61 * f :]


def six_filters(
    m: popcorn.Machine, configs: Sequence[ChainConfig]
) -> tuple[VirtualTime, list[list[int]]]:
    """Filter f on SITF f, on the edge `edge_of(f)`, fed `sweep_bits(f)`, drained by GPDMA1
    channel f into its own buffer of exactly its reference's length. Returns the anchor and
    the references."""
    clock_dma(m)
    expected = [run_chain(sweep_bits(f), config).pcm for f, config in enumerate(configs)]
    for f in range(len(configs)):
        bits = sweep_bits(f)
        asyncio.run(m.periph.MDF1.mic(f, edge_of(f)).write_pdm(pack(bits), bits=len(bits)))
        dma(m, "GPDMA1", f, MDF1_REQUEST + f, dr(MDF1, f), BUFFER + 0x4000 * f, len(expected[f]))
    anchor = cck(m)
    for f, config in enumerate(configs):
        sitf(m, f)
        program(m, f, config, stream=2 * f + (f % 2), cr=DMAEN | DFLTEN)
    return anchor, expected


def last_time(
    anchor: VirtualTime, configs: Sequence[ChainConfig], counts: list[int]
) -> VirtualTime:
    return max(
        sample_time(anchor, config, counts[f] - 1, edge_of(f)) for f, config in enumerate(configs)
    )


@pytest.mark.parametrize("first", range(0, len(SWEEP), 6))
def test_every_order_and_scale_code_on_six_independent_filters_at_once(
    u575: popcorn.PartDescription, first: int
) -> None:
    configs = SWEEP[first : first + 6]
    m = golden_machine(u575)
    anchor, expected = six_filters(m, configs)
    assert m.periph.MDF1.GCR.ILVNB == 0
    run_to(m, last_time(anchor, configs, [len(e) for e in expected]) + us(10))
    for f, config in enumerate(configs):
        want = expected[f]
        got = m.periph.MDF1.output(f).take()[: len(want)]
        assert [s.pcm for s in got] == want, f
        assert [s.at for s in got] == [
            sample_time(anchor, config, k, edge_of(f)) for k in range(len(want))
        ], f
        assert words(m, BUFFER + 0x4000 * f, len(want)) == [value << 8 for value in want], f


INTERLEAVED = ChainConfig(order=4, decimation=16, scale=0x02, rsflt=True, hpf=1)
INTERLEAVED_SAMPLES = 48


def interleaved_bits(f: int) -> list[int]:
    """A -6 dBFS sine at (f + 1) x 500 Hz, held 16 bits per PCM sample."""
    count = INTERLEAVED_SAMPLES * bits_per_sample(INTERLEAVED)
    pcm = tone(-6, 500 * (f + 1), BIT_RATE / 16, count // 16)
    return modulate(pcm, [16] * len(pcm))


def test_six_interleaved_filters_fill_one_buffer_round_by_round(
    u575: popcorn.PartDescription,
) -> None:
    m = golden_machine(u575)
    clock_dma(m)
    mdf = m.periph.MDF1
    total = 6 * INTERLEAVED_SAMPLES
    expected = [run_chain(interleaved_bits(f), INTERLEAVED).pcm for f in range(6)]
    assert all(len(e) == INTERLEAVED_SAMPLES for e in expected)
    for f in range(6):
        bits = interleaved_bits(f)
        asyncio.run(mdf.mic(f, edge_of(f)).write_pdm(pack(bits), bits=len(bits)))
    dma(m, "GPDMA1", 0, MDF1_REQUEST, dr(MDF1, 0), BUFFER, total)
    mdf.GCR.write(5 << 4)
    anchor = cck(m)
    for f in range(6):
        sitf(m, f)
        cr = SYNC_CONTINUOUS | DFLTEN | (DMAEN if f == 0 else 0)
        program(m, f, INTERLEAVED, stream=2 * f + (f % 2), cr=cr)
    mdf.GCR.write((5 << 4) | 1)
    end = sample_time(anchor, INTERLEAVED, INTERLEAVED_SAMPLES - 1, "rising")
    run_to(m, end + us(10))
    rounds = [expected[f][k] << 8 for k in range(INTERLEAVED_SAMPLES) for f in range(6)]
    assert words(m, BUFFER, total) == rounds
    for f in range(6):
        got = mdf.output(f).take()[:INTERLEAVED_SAMPLES]
        assert [s.pcm for s in got] == expected[f], f
        assert [s.at for s in got] == [
            sample_time(anchor, INTERLEAVED, k, edge_of(f)) for k in range(INTERLEAVED_SAMPLES)
        ], f
    assert m.periph.GPDMA1.C0BR1.BNDT == 0


ADF_CHAIN = ChainConfig(order=4, decimation=16, scale=0x0E)
ENVELOPES: dict[str, Callable[[int], int]] = {
    "noise": sad_golden.speech,
    "fixed": sad_golden.bursts(
        300, [(30, 50, 5000), (50, 70, 2500), (90, 95, 0), (120, 140, 2500)]
    ),
    "ambient": sad_golden.ramp,
}


def sad_runs() -> list[tuple[str, int]]:
    return [
        (name, index)
        for name in ENVELOPES
        for index in range(len(json.loads((DATA / f"sad_{name}.json").read_text())["runs"]))
    ]


def sad_registers(config: SadConfig) -> tuple[int, int]:
    """SADCR without SADEN, DATCAP 01, and SADCFGR of `config`."""
    sadcr = (config.sadmod << 12) | (config.frsize << 8) | (int(config.hysten) << 7) | (1 << 1)
    sadcfgr = (
        (config.anmin << 16)
        | (config.hgovr << 12)
        | (config.lfrnb << 8)
        | (config.anslp << 4)
        | config.snthr
    )
    return sadcr, sadcfgr


@pytest.mark.parametrize(("name", "index"), sad_runs(), ids=lambda v: str(v))
def test_the_adf_detector_captures_only_detect_frames_into_lpdma1(
    u575: popcorn.PartDescription, name: str, index: int
) -> None:
    run = json.loads((DATA / f"sad_{name}.json").read_text())["runs"][index]
    config = SadConfig(**run["config"])
    frames = len(run["frames"])
    total = frames * config.frame
    envelope = ENVELOPES[name]
    pcm16 = [
        round(max(envelope(k // config.frame), 0) * math.sin(2 * math.pi * 3000 * k / 128_000))
        for k in range(total)
    ]
    bits = modulate(pcm16, [16] * total)
    pcm = run_chain(bits, ADF_CHAIN).pcm
    detected = sad(pcm, config)
    assert len(detected) == frames
    states = [SadState.LEARN] + [frame.state for frame in detected]
    captured = [states[k // config.frame] == SadState.DETECT for k in range(total)]
    assert 0 < sum(captured) < total
    m = golden_machine(u575)
    clock_dma(m)
    adf = m.periph.ADF1
    asyncio.run(adf.mic(0, "rising").write_pdm(pack(bits), bits=len(bits)))
    dma(m, "LPDMA1", 0, ADF1_REQUEST, dr(ADF1, 0), SRAM4, total)
    sadcr, sadcfgr = sad_registers(config)
    adf.SADCFGR.write(sadcfgr)
    adf.SADCR.write(sadcr)
    adf.SADCR.write(sadcr | 1)
    anchor = cck(m, "ADF1")
    sitf(m, 0, "ADF1")
    program(m, 0, ADF_CHAIN, stream=0, cr=DMAEN | DFLTEN, unit="ADF1")
    run_to(m, sample_time(anchor, ADF_CHAIN, total - 1) + us(5))
    got = adf.output(0).take()[:total]
    assert [s.pcm for s in got] == pcm
    assert [s.captured for s in got] == captured
    assert [s.at for s in got] == [sample_time(anchor, ADF_CHAIN, k) for k in range(total)]
    kept = [value << 8 for value, keep in zip(pcm, captured, strict=True) if keep]
    assert words(m, SRAM4, len(kept)) == kept
    assert 4 * (total - len(kept)) == m.periph.LPDMA1.C0BR1.BNDT
    assert int(detected[-1].state) == adf.SADCR.SADST


RING = 256


def test_the_peer_chain_fills_a_16_bit_ring_with_filter_0_then_filter_1(
    u575: popcorn.PartDescription,
) -> None:
    quiet = json.loads((DATA / "chain_pdm_16k.json").read_text())["runs"][0]
    loud = json.loads((DATA / "chain_pdm_16k_loud.json").read_text())["runs"][0]
    assert decode_config(quiet["config"]) == decode_config(loud["config"]) == PEER
    m = golden_machine(u575)
    clock_dma(m)
    mdf = m.periph.MDF1
    sources: list[tuple[Edge, Json]] = [("falling", quiet), ("rising", loud)]
    for f, (edge, vector) in enumerate(sources):
        bits = unpack(vector["bits"])
        asyncio.run(mdf.mic(f, edge).write_pdm(pack(bits), bits=len(bits)))
    mdf.GCR.write(1 << 4)
    anchor = cck(m)
    for f in range(2):
        sitf(m, f)
    for f in range(2):
        mdf[f"BSMX{f}CR"].write(1 if f == 0 else 2)
        mdf[f"DFLT{f}CICR"].write((0x2C << 20) | (31 << 8) | (5 << 4))
        mdf[f"DFLT{f}RSFR"].write(2 << 8)
    dma(m, "GPDMA1", 6, MDF1_REQUEST, dr(MDF1, 0), BUFFER, RING, half=True, ring=True)
    for f in range(2):
        mdf[f"DFLT{f}CR"].write(SYNC_CONTINUOUS | DMAEN | DFLTEN)
    assert mdf.DFLT0CR.DFLTRUN == 0
    mdf.GCR.write((1 << 4) | 1)
    per_filter = 200
    pcm = [vector_pcm(vector) for _, vector in sources]
    run_to(m, sample_time(anchor, PEER, per_filter - 1, "rising") + us(10))
    sent = [pcm[f][k] >> 8 for k in range(per_filter) for f in range(2)]
    ring = [sent[max(i for i in range(len(sent)) if i % RING == p)] for p in range(RING)]
    assert halves(m, BUFFER, RING) == ring
    assert 4 * (RING - (len(sent) - RING)) == m.periph.GPDMA1.C6BR1.BNDT
    for f, (edge, _) in enumerate(sources):
        got = mdf.output(f).take()[:per_filter]
        assert [s.pcm for s in got] == pcm[f][:per_filter]
        assert [s.at for s in got] == [
            sample_time(anchor, PEER, k, edge) for k in range(per_filter)
        ]


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_parked_and_running_captures_give_the_same_buffers_and_times(
    u575: popcorn.PartDescription, running: bool
) -> None:
    m = golden_machine(u575)
    anchor, expected = six_filters(m, ORDERS)
    taps = [m.periph.MDF1.output(f) for f in range(6)]

    async def scenario() -> list[list[FilterSample]]:
        return [await tap.read(len(expected[f])) for f, tap in enumerate(taps)]

    if running:

        async def free() -> list[list[FilterSample]]:
            async with m.running():
                got = await scenario()
                await taps[0].read(1)
                return got

        got = run_bounded(free(), 20)
    else:
        got = asyncio.run(scenario())
        asyncio.run(taps[0].read(1))
    for f, config in enumerate(ORDERS):
        assert [s.pcm for s in got[f]] == expected[f], f
        assert [s.at for s in got[f]] == [
            sample_time(anchor, config, k, edge_of(f)) for k in range(len(expected[f]))
        ], f
        assert words(m, BUFFER + 0x4000 * f, len(expected[f])) == [v << 8 for v in expected[f]]
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def model_state(saved: popcorn.Snapshot, owner: str = "MDF1") -> bytes:
    models = cast(dict[str, dict[str, object]], bson(saved.to_bytes())["models"])
    for part in models.values():
        if part["owner"] == owner:
            return cast(bytes, part["data"])
    raise AssertionError(f"no {owner} state")


def mirrored(saved: bytes) -> MdfState:
    """The mirror of a model's state, checklist 18: it decodes and dumps back to the bytes."""
    state = decode(saved)
    assert encode(state) == saved
    return state


SNAPSHOT_HZ = 16_000
WATCHED = 8
SPLIT = ChainConfig(order=1, decimation=100)


def snapshot_leg(
    u575: popcorn.PartDescription, running: bool
) -> tuple[VirtualTime, bytes, list[FilterSample]]:
    """Filter 0 runs the peer chain on PCM, filter 1 Sinc1 D 100 on the same stream. A watch on
    filter 1's ninth DMA word stops the run between filter 0's samples 7 and 8. 20 us later,
    with bits clocked that no sample pulled yet, the leg snapshots, restores and writes other
    PCM. Returns the snapshot's time, the model state it
    holds and the samples after the restore."""
    m = golden_machine(u575)
    clock_dma(m)
    first_pcm = tone(-12, 1000, SNAPSHOT_HZ, 160)
    second_pcm = tone(-9, 700, SNAPSHOT_HZ, 160)
    mic = m.periph.MDF1.mic(0, "rising")
    tap = m.periph.MDF1.output(0)
    asyncio.run(mic.write_pcm(first_pcm, hz=SNAPSHOT_HZ))
    dma(m, "GPDMA1", 1, MDF1_REQUEST + 1, dr(MDF1, 1), BUFFER + 0x4000, 64)
    m.watch(BUFFER + 0x4000 + 4 * WATCHED, 4)
    anchor = cck(m)
    sitf(m, 0)
    program(m, 0, PEER, stream=0, cr=DFLTEN)
    program(m, 1, SPLIT, stream=0, cr=DMAEN | DFLTEN)
    if running:

        async def free() -> None:
            with pytest.raises(popcorn.EmulationError):
                async with m.running():
                    await tap.read(1000)

        run_bounded(free(), 20)
        assert m.last_stop is not None and m.last_stop.reason is StopReason.WATCHPOINT
    else:
        assert asyncio.run(m.run()).reason is StopReason.WATCHPOINT
    m.remove_hooks()
    assert sample_time(anchor, SPLIT, WATCHED) <= m.time < sample_time(anchor, SPLIT, WATCHED + 1)
    asyncio.run(m.run(for_=us(20)))
    at = m.time
    assert at < sample_time(anchor, PEER, 8)
    consumed = rising_edges_by(anchor, at)
    assert consumed % 128
    saved = m.snapshot()
    data = model_state(saved)
    state = mirrored(data)
    assert state.filter[0].running and state.filter[0].chain.cic.integ != [0] * 5
    assert state.sitf[0].next == sum(edge_at(anchor, h) <= at for h in range(1, 4 * consumed)) + 1
    assert len(tap.take()) == 7
    m.restore(saved)
    asyncio.run(mic.write_pcm(second_pcm, hz=SNAPSHOT_HZ))
    stream = modulate(first_pcm, held_bits(len(first_pcm), SNAPSHOT_HZ))[:consumed]
    stream += modulate(second_pcm, held_bits(len(second_pcm), SNAPSHOT_HZ))
    run_to(m, sample_time(anchor, PEER, 39) + us(10))
    got = tap.take()
    assert [s.pcm for s in got] == run_chain(stream, PEER).pcm[7:40]
    assert [s.at for s in got] == [sample_time(anchor, PEER, k) for k in range(7, 40)]
    return at, data, got


def test_a_snapshot_mid_sample_restores_the_chain_and_a_fresh_modulator_parked_or_running(
    u575: popcorn.PartDescription,
) -> None:
    assert snapshot_leg(u575, running=True) == snapshot_leg(u575, running=False)


def rising_edges_by(anchor: VirtualTime, t: VirtualTime) -> int:
    """Bits of a rising-edge stream clocked at or before `t`."""
    count = 0
    while edge_at(anchor, 2 + 2 * count) <= t:
        count += 1
    return count


def start_capture(m: popcorn.Machine, unit: str, bits: list[int]) -> VirtualTime:
    """The peer chain on `unit`'s filter 0, SITF0 rising, into a GPDMA1 or LPDMA1 buffer."""
    clock_dma(m)
    asyncio.run(m.periph[unit].mic(0, "rising").write_pdm(pack(bits), bits=len(bits)))
    if unit == "MDF1":
        dma(m, "GPDMA1", 0, MDF1_REQUEST, dr(MDF1, 0), BUFFER, 200)
    else:
        dma(m, "LPDMA1", 0, ADF1_REQUEST, dr(ADF1, 0), SRAM4, 200)
    anchor = cck(m, unit)
    sitf(m, 0, unit)
    program(m, 0, PEER, stream=0, cr=DMAEN | DFLTEN, unit=unit)
    return anchor


def capture_of(m: popcorn.Machine, unit: str) -> tuple[list[FilterSample], list[int]]:
    return m.periph[unit].output(0).take(), words(m, BUFFER if unit == "MDF1" else SRAM4, 200)


UNITS = {"a": "MDF1", "b": "ADF1"}


def test_two_system_members_capture_on_mdf1_and_adf1_as_lone_machines_and_restore(
    u575: popcorn.PartDescription,
) -> None:
    data, count, _ = pdm_16k()
    bits = bits_of(data, count)
    alone: dict[str, tuple[list[FilterSample], list[int]]] = {}
    for name, unit in UNITS.items():
        m = golden_machine(u575, name)
        anchor = start_capture(m, unit, bits)
        run_to(m, sample_time(anchor, PEER, 199) + us(10))
        alone[name] = capture_of(m, unit)
        assert [s.pcm for s in alone[name][0][:200]] == run_chain(bits, PEER).pcm[:200]
    members = {name: golden_machine(u575, name) for name in UNITS}
    system = System(members, [UartLink("a.USART1", "b.USART1", latency=us(10), baud=460_800)])
    try:
        anchors = {name: start_capture(members[name], unit, bits) for name, unit in UNITS.items()}
        anchor = anchors["a"]
        assert anchors["b"] == anchor
        a = members["a"]
        asyncio.run(system.run(for_=anchor + us(62.5 * 9 + 20) - a.time))
        consumed = {name: rising_edges_by(anchor, m.time) for name, m in members.items()}
        assert all(c % 128 for c in consumed.values())
        before = {name: m.periph[UNITS[name]].output(0).take() for name, m in members.items()}
        taken = system.snapshot()
        end = sample_time(anchor, PEER, 199) + us(10)
        asyncio.run(system.run(for_=end - a.time))
        once = {name: capture_of(m, UNITS[name]) for name, m in members.items()}
        for name in UNITS:
            assert before[name] + once[name][0] == alone[name][0], name
            assert once[name][1] == alone[name][1], name
        system.restore(taken)
        for name, m in members.items():
            rest = bits[consumed[name] :]
            mic = m.periph[UNITS[name]].mic(0, "rising")
            asyncio.run(mic.write_pdm(pack(rest), bits=len(rest)))
        asyncio.run(system.run(for_=end - a.time))
        assert {name: capture_of(m, UNITS[name]) for name, m in members.items()} == once
    finally:
        system.close()


def test_a_system_reset_then_a_power_on_reset_keeps_input_and_restarts_in_the_new_base(
    u575: popcorn.PartDescription,
) -> None:
    data, count, _ = pdm_16k()
    m = golden_machine(u575)
    registers = m.periph.MDF1.registers
    fresh = {name: m.periph.MDF1[name].read() for name in registers}
    clock_dma(m)
    mic = m.periph.MDF1.mic(0, "rising")
    asyncio.run(mic.write_pdm(data, bits=count))
    anchor = cck(m)
    sitf(m, 0)
    program(m, 0, PEER, stream=0, cr=DFLTEN)
    run_to(m, sample_time(anchor, PEER, 3) + us(20))
    consumed = rising_edges_by(anchor, m.time)
    assert len(m.periph.MDF1.output(0).take()) == 4
    m.reset(ResetKind.SOFTWARE)
    assert {name: m.periph.MDF1[name].read() for name in registers} == fresh
    m.reg.pc = m.sym("idle")
    asyncio.run(m.run(for_=us(50)))
    m.reset(ResetKind.POWER_ON)
    assert {name: m.periph.MDF1[name].read() for name in registers} == fresh
    assert mic.queued_bits == count - consumed
    clocked(m)
    fast_bus(m)
    clock_dma(m)
    dma(m, "GPDMA1", 0, MDF1_REQUEST, dr(MDF1, 0), BUFFER, 32)
    anchor = cck(m)
    assert anchor < VirtualTime(0) + us(1000)
    sitf(m, 0)
    program(m, 0, PEER, stream=0, cr=DMAEN | DFLTEN)
    want = run_chain(bits_of(data, count)[consumed:], PEER).pcm[:32]
    run_to(m, sample_time(anchor, PEER, 31) + us(10))
    got = m.periph.MDF1.output(0).take()[:32]
    assert [s.pcm for s in got] == want
    assert [s.at for s in got] == [sample_time(anchor, PEER, k) for k in range(32)]
    assert words(m, BUFFER, 32) == [v << 8 for v in want]

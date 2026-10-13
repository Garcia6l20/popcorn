"""The ADC1 analog watchdog end to end: a ten-rank TIM15-triggered scan with DMA and AWD3 on IN3.

The LL test firmware's `scan_awd` runs ADC1 on PLL2R at 72 MHz: 12-bit, 8x oversampling shifted
by 3, a circular GPDMA list, one scan per TIM15 update every 500 ms, AWD3 guarding IN3 with the
thresholds of a plug detector. Its ADC1 handler runs a deferred plug callback at the end of
scan, after an AWD3 interrupt set `detected`, then swaps the thresholds. Log words carry a tag in
the top byte and a TIM2 count at 1 MHz, or the scan count, below it."""

from __future__ import annotations

import asyncio
import struct
from pathlib import Path

import popcorn
from popcorn import StopReason
from popcorn.board import SupplyOptions
from popcorn.units import ms, ps, us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
ADC_ELF = PREBUILT / "adc_ll.elf"
BUDGET = 100_000_000
PERIOD = ms(500)
VDDA_MV = 3300
TS_CAL1 = 4107
TS_CAL2 = 5472
VREFINT_CAL = 6635
STAMP = 0x00FF_FFFF
START = 0x5A00_0000
AWD3 = 0xA300_0000
EOS = 0xE500_0000
EVENT = 0xCB00_0000
OVERRUN = 0x0E00_0000
HALF = 0xD100_0000
DONE = 0xDC00_0000
DMA_ERROR = 0xDE00_0000
RANKS = (2, 3, 4, 5, 6, 14, 15, 16, 19, 0)
IDLE_IN3 = 0.026
SEATED_IN3 = 2.72
AWD3_AFTER_TRIGGER_PS = 11_375_000
EOS_AFTER_TRIGGER_PS = 233_152_778
AWD3_STAMP = 11
HALF_STAMP = 31
EOS_STAMP = 233
EDGE_PS = 12_500


def tenths(channel: int) -> float:
    return channel / 10


def code12(microvolts: int) -> int:
    return microvolts * 16384 // (VDDA_MV * 1000) >> 2


def expected_results(in3: float) -> list[int]:
    codes: list[int] = []
    for channel in RANKS:
        if channel == 19:
            codes.append(TS_CAL1 * 3000 // VDDA_MV >> 2)
        elif channel == 0:
            codes.append(VREFINT_CAL * 3000 // VDDA_MV >> 2)
        elif channel == 3:
            codes.append(code12(round(in3 * 1e6)))
        else:
            codes.append(code12(round(tenths(channel) * 1e6)))
    return codes


class Scan:
    """A machine running `scan_awd`, stopped right after TIM15 started counting."""

    def __init__(self, u575: popcorn.PartDescription) -> None:
        self.m = popcorn.Machine(
            u575, image=popcorn.Image.elf(ADC_ELF), supply=SupplyOptions(vdda_mv=VDDA_MV)
        )
        self.m.reset()
        assert asyncio.run(self.m.run(until="main", budget=BUDGET)).reason is StopReason.ADDRESS
        self.m.reg.pc = self.m.sym("scan_awd")
        for channel in RANKS:
            if 1 <= channel <= 17:
                self.m.periph.ADC1.input(channel).volts = tenths(channel)
        self.in3 = IDLE_IN3
        stop = asyncio.run(self.m.run(until="scanning", budget=BUDGET))
        assert stop.reason is StopReason.ADDRESS, stop
        self.counting = self.m.time.ps

    @property
    def in3(self) -> float:
        return self.m.periph.ADC1.input(3).volts

    @in3.setter
    def in3(self, volts: float) -> None:
        self.m.periph.ADC1.input(3).volts = volts

    def trigger_ps(self, scan: int) -> int:
        return self.counting + scan * PERIOD.ps

    def run_to(self, at_ps: int) -> None:
        stop = asyncio.run(self.m.run(for_=ps(at_ps - self.m.time.ps)))
        assert stop.reason is StopReason.TIME, stop

    def run_until_flag(self, flag: str) -> int:
        stop = asyncio.run(
            self.m.run(
                for_=PERIOD,
                stop_when=lambda m: getattr(m.periph.ADC1.ADC_ISR, flag) == 1,
            )
        )
        assert stop.reason is StopReason.USER, stop
        return self.m.time.ps

    def between_scans(self, scan: int) -> None:
        self.run_to(self.trigger_ps(scan) + PERIOD.ps // 5)

    def log(self) -> list[int]:
        count = self.m.mem.u32[self.m.sym("log_len")]
        assert count <= 256
        raw = self.m.mem.read(self.m.sym("log_buf"), 4 * count)
        return list(struct.unpack(f"<{count}I", raw))

    def tagged(self, tag: int) -> list[int]:
        return [word & STAMP for word in self.log() if word & ~STAMP == tag]

    def since_start(self, tag: int) -> list[int]:
        (start,) = self.tagged(START)
        return [stamp - start for stamp in self.tagged(tag)]

    def events(self) -> list[tuple[int, int]]:
        return [(word >> 16, word & 0xFFFF) for word in self.tagged(EVENT)]

    def last(self) -> list[int]:
        words = self.m.var("last")
        return [int(words[rank].value) for rank in range(len(RANKS))]

    def check_dma(self, scans: int) -> None:
        assert self.m.var("scans").value == scans
        assert self.m.var("ends").value == scans
        assert len(self.tagged(HALF)) == scans
        assert len(self.tagged(DONE)) == scans
        assert self.tagged(OVERRUN) == []
        assert self.tagged(DMA_ERROR) == []


def near(stamps: list[int], expected: list[int]) -> bool:
    return len(stamps) == len(expected) and all(
        abs(stamp - want) <= 1 for stamp, want in zip(stamps, expected, strict=True)
    )


def at_scans(scans: range | list[int], offset: int) -> list[int]:
    return [scan * PERIOD.ps // 1_000_000 + offset for scan in scans]


def test_the_scan_reads_the_factory_words_and_converts_every_500_ms(
    u575: popcorn.PartDescription,
) -> None:
    scan = Scan(u575)
    scan.run_to(scan.trigger_ps(4) + ms(100).ps)
    assert scan.log()[:3] == [TS_CAL1, TS_CAL2, VREFINT_CAL]
    assert near(scan.since_start(HALF), at_scans(range(1, 5), HALF_STAMP))
    assert near(scan.since_start(EOS), at_scans(range(1, 5), EOS_STAMP))
    assert near(scan.since_start(DONE), at_scans(range(1, 5), EOS_STAMP))
    assert scan.tagged(AWD3) == []
    assert scan.events() == []
    scan.check_dma(4)
    assert scan.last() == expected_results(IDLE_IN3)


def test_a_plug_flags_awd3_at_rank_two_and_the_callback_runs_at_the_end_of_that_scan(
    u575: popcorn.PartDescription,
) -> None:
    scan = Scan(u575)
    scan.between_scans(1)
    scan.in3 = SEATED_IN3
    flagged = scan.run_until_flag("AWD3") - scan.trigger_ps(2)
    assert abs(flagged - AWD3_AFTER_TRIGGER_PS) <= EDGE_PS
    assert scan.m.periph.ADC1.ADC_ISR.EOS == 0
    ended = scan.run_until_flag("EOS") - scan.trigger_ps(2)
    assert abs(ended - EOS_AFTER_TRIGGER_PS) <= EDGE_PS
    scan.between_scans(2)
    assert near(scan.since_start(AWD3), at_scans([2], AWD3_STAMP))
    assert near(scan.since_start(EOS), at_scans([1, 2], EOS_STAMP))
    assert scan.events() == [(1, 2)]
    log = scan.log()
    assert log.index(EVENT | 1 << 16 | 2) == log.index(EOS | scan.tagged(EOS)[1]) + 1
    assert scan.last() == expected_results(SEATED_IN3)

    scan.in3 = 1.45
    scan.between_scans(3)
    assert scan.events() == [(1, 2)]
    scan.in3 = 1.39
    scan.between_scans(4)
    assert scan.events() == [(1, 2), (0, 4)]
    assert near(scan.since_start(AWD3), at_scans([2, 4], AWD3_STAMP))
    scan.check_dma(4)


def test_awd3_trips_strictly_past_the_thresholds_one_microvolt_apart(
    u575: popcorn.PartDescription,
) -> None:
    scan = Scan(u575)
    steps: list[tuple[float, list[tuple[int, int]]]] = [
        (1.499340, []),
        (1.499341, [(1, 2)]),
        (1.398633, [(1, 2)]),
        (1.398632, [(1, 2), (0, 4)]),
    ]
    for index, (volts, events) in enumerate(steps):
        scan.between_scans(index)
        scan.in3 = volts
        scan.between_scans(index + 1)
        assert scan.events() == events, volts
    scan.check_dma(4)


def test_with_interrupts_masked_across_the_scan_the_callback_runs_one_scan_later(
    u575: popcorn.PartDescription,
) -> None:
    scan = Scan(u575)
    scan.between_scans(1)
    scan.in3 = SEATED_IN3
    scan.run_to(scan.trigger_ps(2) - us(1).ps)
    scan.m.reg.primask = 1
    scan.run_to(scan.trigger_ps(2) + us(299).ps)
    assert scan.m.periph.ADC1.ADC_ISR.AWD3 == 1
    assert scan.m.periph.ADC1.ADC_ISR.EOS == 1
    scan.m.reg.primask = 0
    scan.between_scans(2)
    entry = scan.since_start(EOS)[1]
    assert scan.since_start(AWD3) == [entry]
    assert scan.events() == []
    scan.between_scans(3)
    assert scan.events() == [(1, 3)]
    assert near(scan.since_start(AWD3), [entry, *at_scans([3], AWD3_STAMP)])
    scan.check_dma(3)


def test_a_snapshot_between_awd3_and_the_end_of_scan_keeps_the_pending_callback(
    u575: popcorn.PartDescription,
) -> None:
    scan = Scan(u575)
    scan.between_scans(1)
    scan.in3 = SEATED_IN3
    scan.run_to(scan.trigger_ps(2) + us(100).ps)
    assert scan.m.var("detected").value == 1
    saved = scan.m.snapshot()
    scan.between_scans(2)
    assert scan.events() == [(1, 2)]

    other = Scan(u575)
    other.m.restore(popcorn.Snapshot.from_bytes(saved.to_bytes()))
    other.counting = scan.counting
    other.between_scans(2)
    assert other.events() == [(1, 2)]
    assert other.last() == expected_results(SEATED_IN3)
    other.check_dma(2)

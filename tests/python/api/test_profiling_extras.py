"""Watchdog margin, milestones, JSON, folded stacks and diff of `popcorn.profiling`."""

from __future__ import annotations

import asyncio
import dataclasses
import io
import itertools
import json
import re
import types
import typing
from collections.abc import Sequence
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import _core, trace
from popcorn.profiling import SCHEMA, Analysis, FrameKind, Report, diff
from popcorn.profiling._diff import IDENTITY, MEASURES
from popcorn.profiling._symbols import Names
from popcorn.profiling._watchdog import (
    RELOAD_KEYS,
    WatchdogRegisters,
    watchdog,
    watchdog_registers,
)
from popcorn.units import VirtualDuration, ms, s

from .conftest import ROOT
from .test_profiling import SPI, TIM, booted

IWDG = popcorn.MachineDescription(
    name="iwdg-refresh",
    part="STM32U575QGIx",
    images=[popcorn.Elf(ROOT / "examples/stm32u575/iwdg/prebuilt/iwdg_refresh.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)
TIMEOUT_PS = ms(500).ps


def test_the_watchdog_margin_is_the_timeout_less_the_longest_refresh_interval() -> None:
    m = booted(IWDG)
    seen: list[int] = []

    def record(call: popcorn.Call) -> popcorn.Return:
        seen.append(m.time.ps)
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", record)
    start = m.time.ps
    with m.profile("watchdog") as profile:
        stop = asyncio.run(m.run(for_=ms(1050)))
    assert stop.reason is popcorn.StopReason.TIME
    dog = profile.report().watchdog
    assert dog is not None
    assert (dog.starts, dog.refreshes) == (1, len(seen) + 1)
    assert dog.timeout_ps == TIMEOUT_PS
    longest = max(b - a for a, b in itertools.pairwise(seen))
    assert dog.min_margin_ps == TIMEOUT_PS - longest == 398_998_750_000
    assert dog.closest_pc is not None
    assert Names(m.images, m.description).function(dog.closest_pc)[0] == "HAL_IWDG_Refresh"
    assert dog.gaps == ()
    assert dog.logged_until_ps is None
    assert seen[0] - start < ms(1).ps


def advance(
    m: popcorn.Machine,
    span: VirtualDuration,
    reason: popcorn.StopReason = popcorn.StopReason.TIME,
) -> None:
    stop = asyncio.run(m.run(for_=span))
    assert stop.reason is reason, stop


def test_a_stopped_refresh_shows_as_a_gap_of_the_timeout() -> None:
    m = booted(IWDG)
    with m.profile("watchdog") as profile:
        advance(m, ms(250))
        m.var("refresh_enabled").value = 0
        advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
    report = profile.report()
    dog = report.watchdog
    assert dog is not None
    assert len(dog.gaps) == 1
    gap = dog.gaps[0]
    assert (gap.length_ps, gap.timeout_ps) == (TIMEOUT_PS, TIMEOUT_PS)
    assert gap.start_ps + gap.length_ps == report.elapsed_ps
    assert "no reload for 500.000 ms" in report.text()


def test_reloads_on_either_side_of_a_watchdog_reset_are_not_paired() -> None:
    m = booted(IWDG)
    with m.profile("watchdog") as profile:
        advance(m, ms(250))
        m.var("refresh_enabled").value = 0
        advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
        m.reset(popcorn.ResetKind.WATCHDOG)
        advance(m, ms(350))
    report = profile.report()
    assert report.cuts == 1
    dog = report.watchdog
    assert dog is not None
    assert dog.starts == 2
    assert dog.min_margin_ps == 398_998_750_000
    assert [gap.length_ps for gap in dog.gaps] == [TIMEOUT_PS]


def test_a_reset_at_a_set_core_rate_sends_the_core_rate_after_the_rebase_only(
    tmp_path: Path,
) -> None:
    m = booted(TIM.replace(core_hz=16_000_000))
    asyncio.run(m.run(budget=50_000))
    path = tmp_path / "reset.pctr"
    with m.trace(trace.ALL, path), m.profile("sleep") as profile:
        m.reset()
        asyncio.run(m.run(budget=1_000))
    rates = [
        (type(record).__name__, record.time)
        for record in trace.read(path)
        if isinstance(record, trace.Clock | trace.Rebase)
    ]
    assert [name for name, _ in rates] == ["Clock", "Rebase", "Clock"]
    assert rates[1:] == [("Rebase", 0), ("Clock", 0)]
    report = profile.report()
    assert report.cuts == 1
    assert report.elapsed_ps == m.time.ps
    assert sum(span.ps for span in report.clocks) == report.elapsed_ps


def test_a_part_firmware_with_no_watchdog_reports_no_reload() -> None:
    m = booted(TIM)
    with m.profile("watchdog") as profile:
        asyncio.run(m.run(for_=ms(5)))
    dog = profile.report().watchdog
    assert dog is not None
    assert (dog.refreshes, dog.starts, dog.timeout_ps, dog.min_margin_ps, dog.gaps) == (
        0,
        0,
        None,
        None,
        (),
    )


def boot_time(symbol: str) -> int:
    m = TIM.machine()
    m.reset()
    stop = asyncio.run(m.run(until=symbol, budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    return m.time.ps


def test_milestones_time_the_first_block_at_each_symbol_or_address() -> None:
    m = TIM.machine()
    m.reset()
    main = m.sym("main") & ~1
    with m.profile("sleep", milestones=["SystemInit", main, "TIM2_IRQHandler", 0x0800_0000]) as p:
        asyncio.run(m.run(for_=ms(5)))
    report = p.report()
    assert report.timeline is not None
    assert [(milestone.name, milestone.address) for milestone in report.timeline] == [
        ("SystemInit", m.sym("SystemInit") & ~1),
        (f"{main:#010x}", main),
        ("TIM2_IRQHandler", m.sym("TIM2_IRQHandler") & ~1),
        ("0x08000000", 0x0800_0000),
    ]
    times = [milestone.time_ps for milestone in report.timeline]
    assert times[0] == boot_time("SystemInit")
    assert times[1] == boot_time("main")
    assert times[1] is not None
    assert times[2] is not None and times[2] > times[1]
    assert times[3] is None
    assert "== timeline ==" in report.text()


def test_milestones_are_refused_at_the_call() -> None:
    m = booted(TIM)
    with pytest.raises(popcorn.SymbolError), m.profile(milestones=["no_such_symbol"]):
        pass
    with pytest.raises(ValueError, match="must lie"), m.profile(milestones=[-4]):
        pass
    with pytest.raises(ValueError, match="must lie"), m.profile(milestones=[1 << 32]):
        pass
    with pytest.raises(TypeError, match="symbol name or an address"), m.profile(milestones=[True]):
        pass
    assert m._handle.loop_reports() == 0  # pyright: ignore[reportPrivateUsage]
    with m.profile() as profile:
        pass
    assert profile.report().timeline is None


def spi_report(span: VirtualDuration) -> Report:
    m = booted(SPI)
    with m.profile(milestones=["prvIdleTask"]) as profile:
        asyncio.run(m.run(for_=span))
    return profile.report()


def test_the_json_holds_the_schema_and_reads_back_equal() -> None:
    report = spi_report(ms(20))
    text = report.to_json()
    data = json.loads(text)
    assert data["schema"] == SCHEMA == 2
    assert data["report"]["hotspots"]["frames"][1]["kind"] == FrameKind.ENTRY.value
    assert data["report"]["analyses"] == [a.value for a in report.analyses]
    back = Report.from_json(text)
    assert back == report
    assert back.text() == report.text()
    assert isinstance(back.analyses[0], Analysis)
    with pytest.raises(ValueError, match="schema 2"):
        Report.from_json(json.dumps({"schema": 1, "report": data["report"]}))
    with pytest.raises(ValueError, match="malformed"):
        Report.from_json(json.dumps({"schema": 2, "report": {}}))


def test_folded_stacks_have_one_line_per_node_with_its_self_instructions() -> None:
    report = spi_report(ms(20))
    out = io.StringIO()
    count = report.write_folded(out)
    lines = out.getvalue().splitlines()
    assert count == len(lines) > 0
    shape = re.compile(r"^[^;\n]+(;[^;\n]+)* \d+$")
    assert all(shape.match(line) for line in lines)
    assert sum(int(line.rsplit(" ", 1)[1]) for line in lines) == report.executed
    assert any("[SPI1]" in line for line in lines)
    assert any(line.startswith("prvIdleTask;prvCheckTasksWaitingTermination ") for line in lines)
    sleepy = booted(TIM)
    with sleepy.profile("sleep") as profile:
        pass
    with pytest.raises(ValueError, match="hotspots"):
        profile.report().write_folded(io.StringIO())


def test_the_folded_file_equals_the_stream(tmp_path: object) -> None:
    from pathlib import Path

    assert isinstance(tmp_path, Path)
    report = spi_report(ms(5))
    out = io.StringIO()
    report.write_folded(out)
    target = tmp_path / "spi.folded"
    report.write_folded(target)
    assert target.read_text(encoding="utf-8") == out.getvalue()


def test_a_report_against_itself_differs_in_nothing() -> None:
    report = spi_report(ms(20))
    assert diff(report, report) == {}
    assert diff(report, Report.from_json(report.to_json())) == {}


def test_the_diff_gives_b_less_a_per_key() -> None:
    short = spi_report(ms(10))
    long = spi_report(ms(20))
    changes = diff(short, long)
    summary = changes["summary"]["report"]
    assert summary["elapsed_ps"] == long.elapsed_ps - short.elapsed_ps
    assert summary["executed"] == long.executed - short.executed
    assert "cuts" not in summary
    assert short.hotspots is not None and long.hotspots is not None
    check = "prvCheckTasksWaitingTermination"
    before = next(row for row in short.hotspots.functions if row.name == check)
    after = next(row for row in long.hotspots.functions if row.name == check)
    assert changes["functions"][check]["self"] == after.self - before.self
    assert short.irq is not None and long.irq is not None
    pendsv = {row.name: row for row in short.irq.exceptions}["PendSV"]
    pendsv_after = {row.name: row for row in long.irq.exceptions}["PendSV"]
    assert pendsv_after.entries > pendsv.entries
    assert changes["exceptions"]["PendSV"]["entries"] == pendsv_after.entries - pendsv.entries
    spi1 = {row.name: row.entries for row in short.irq.exceptions}["SPI1"]
    assert spi1 == {row.name: row.entries for row in long.irq.exceptions}["SPI1"]
    assert "entries" not in changes["exceptions"].get("SPI1", {})
    reverse = diff(long, short)
    grown = summary["elapsed_ps"]
    assert isinstance(grown, int)
    assert reverse["summary"]["report"]["elapsed_ps"] == -grown
    assert diff(short, short) == {}


def test_a_milestone_reached_in_one_report_only_maps_to_its_time() -> None:
    m = TIM.machine()
    m.reset()
    with m.profile("sleep", milestones=["main"]) as early:
        asyncio.run(m.run(until="SystemInit", budget=1_000_000))
    with m.profile("sleep", milestones=["main"]) as late:
        asyncio.run(m.run(for_=ms(1)))
    changes = diff(early.report(), late.report())
    late_main = late.report().timeline
    assert late_main is not None and late_main[0].time_ps is not None
    assert changes["timeline"]["main"]["time_ps"] == late_main[0].time_ps
    assert diff(late.report(), early.report())["timeline"]["main"]["time_ps"] is None


def test_a_function_pointer_milestone_reaches_its_function() -> None:
    m = TIM.machine()
    m.reset()
    pointer = m.fn_ptr("main")
    assert pointer & 1
    with m.profile("sleep", milestones=["main", pointer]) as profile:
        asyncio.run(m.run(for_=ms(1)))
    timeline = profile.report().timeline
    assert timeline is not None
    assert timeline[1].address == pointer & ~1
    assert timeline[1].time_ps == timeline[0].time_ps == boot_time("main")


def iwdg_writes(
    chunk: VirtualDuration | None,
) -> tuple[list[tuple[int, int, int, int]], list[tuple[int, int, int]]]:
    m = booted(IWDG)
    dog = watchdog_registers(m.description, lambda at: int.from_bytes(m.mem.read(at, 4), "little"))
    assert dog is not None
    with m._profile(  # pyright: ignore[reportPrivateUsage]
        logged_registers=dog.logged, write_clock_domain="lsi"
    ) as recording:
        if chunk is None:
            stops = [asyncio.run(m.run(for_=ms(2)))]
        else:
            stops = [asyncio.run(m.run(for_=chunk)) for _ in range(ms(2).ps // chunk.ps)]
        assert {stop.reason for stop in stops} == {popcorn.StopReason.TIME}
    result = recording.result
    writes = sorted(
        (write.at, write.value, write.ps, write.clock_hz)
        for log in result.write_logs
        for write in log.writes
    )
    rates = [(rate.at, rate.ps, rate.hz) for rate in result.domain_rates]
    return writes, rates


def test_a_logged_write_takes_the_lsi_rate_of_its_own_instant_whatever_the_slicing() -> None:
    whole = iwdg_writes(None)
    sliced = iwdg_writes(VirtualDuration(ps=ms(1).ps // 40))
    assert whole == sliced
    writes, rates = whole
    start = next(write for write in writes if write[1] == 0xCCCC)
    assert start[3] == 0
    assert [hz for _, _, hz in rates] == [0, 32_000]
    assert all(write[3] == 32_000 for write in writes if write[2] > rates[1][1])


LSI_HZ = 32_000
PERIOD_PS = 1_000_000_000_000 // LSI_HZ
KR, PR, RLR = 1, 2, 3
REGISTERS = WatchdogRegisters(
    kr=(KR,),
    pr=(PR,),
    rlr=(RLR,),
    prescaler_top=8,
    pr_mask=0xF,
    rlr_mask=0xFFF,
    pr_start=0,
    rlr_start=0xFFF,
)

type Event = tuple[str, int, int, int]
"""`("w", register, value, ps)` or `("r", 0, hz, ps)`, in stream order; a `("cut", 0, 0, ps)`
starts the next segment."""


class _Mirror:
    """The reload tracker rule of the profile sink, written again in Python over `Event`s."""

    def __init__(self, limit: int) -> None:
        self.limit = limit
        self.track = _core.ProfileReloadTrack()
        self.track.counts = [0, 0]
        self.epochs: list[_core.ProfileEpoch] = [self.epoch(_core.EpochKind.start, 0, 0, 0)]
        self.best: dict[tuple[int, int], int] = {}
        self.tails: list[_core.ProfileReloadTail] = []
        self.last_key: int | None = None
        self.previous: tuple[int, int, int] | None = None
        self.ticks = 0
        self.hz = 0
        self.last_rate_ps = 0
        self.frozen = False

    @staticmethod
    def epoch(kind: _core.EpochKind, segment: int, ps: int, record: int) -> _core.ProfileEpoch:
        epoch = _core.ProfileEpoch()
        epoch.kind, epoch.segment, epoch.ps, epoch.record = kind, segment, ps, record
        epoch.key_before = 0xFFFF_FFFF
        epoch.longest = [None, None]
        return epoch

    def open(self, epoch: _core.ProfileEpoch) -> None:
        if len(self.epochs) >= self.limit:
            if self.track.epochs_dropped == 0:
                self.track.frozen_ps, self.track.frozen_segment = epoch.ps, epoch.segment
                self.frozen = True
            self.track.epochs_dropped += 1
            return
        self.epochs.append(epoch)

    def rate(self, hz: int, ps: int) -> None:
        self.ticks += self.hz * (ps - self.last_rate_ps)
        self.last_rate_ps, self.hz = ps, hz

    def cut(self, segment: int, ps: int, record: int) -> None:
        self.open(self.epoch(_core.EpochKind.cut, segment, ps, record))
        self.previous = None
        self.last_key = None

    def change(self, register: int, write: _core.ProfileWrite) -> None:
        epoch = self.epoch(_core.EpochKind.change, write.segment, write.ps, write.record)
        epoch.change_id, epoch.change = register, write
        if self.last_key is not None:
            epoch.key_before = self.last_key
        self.open(epoch)

    def key(self, write: _core.ProfileWrite) -> None:
        self.track.key_writes += 1
        self.last_key = write.value & 0xFFFF
        if self.last_key not in RELOAD_KEYS:
            return
        kind = RELOAD_KEYS.index(self.last_key)
        self.track.counts = [
            count + (index == kind) for index, count in enumerate(self.track.counts)
        ]
        if self.frozen:
            return
        ticks = self.ticks + self.hz * (write.ps - self.last_rate_ps)
        if self.previous is not None:
            first_kind, first_epoch, first_ticks = self.previous
            gap = ticks - first_ticks
            slot = self.epochs[first_epoch]
            if slot.longest[first_kind] is None or gap > self.best[first_epoch, first_kind]:
                held = _core.ProfileReloadGap()
                held.start, held.end = self.tails[-1].last, write
                held.ps = write.ps - held.start.ps
                longest = list(slot.longest)
                longest[first_kind] = held
                slot.longest = longest
                self.best[first_epoch, first_kind] = gap
        tail = _core.ProfileReloadTail()
        tail.segment, tail.last, tail.kind = write.segment, write, kind
        tail.epoch = len(self.epochs) - 1
        if self.tails and self.tails[-1].segment == write.segment:
            self.tails[-1] = tail
        else:
            self.tails.append(tail)
        self.previous = (kind, tail.epoch, ticks)

    def finish(self) -> _core.ProfileReloadTrack:
        self.track.epochs = self.epochs
        self.track.tails = self.tails
        return self.track


def synthetic(
    events: list[Event],
    end_ps: int,
    limit: int = 4096,
    keep_logs: bool = True,
    pcs: Sequence[int] | None = None,
) -> _core.ProfileResult:
    result = _core.ProfileResult()
    logs = {register: _core.ProfileWriteLog() for register in (KR, PR, RLR)}
    rates: list[_core.ProfileRate] = []
    clocks: list[_core.ProfileClock] = []
    mirror = _Mirror(limit)
    records = segment = 0
    segment_start = 0
    for kind, register, value, ps in events:
        if kind == "cut":
            clock = _core.ProfileClock()
            clock.segment, clock.ps = segment, ps - segment_start
            clocks.append(clock)
            segment, segment_start = segment + 1, ps
            mirror.cut(segment, ps, records)
        elif kind == "r":
            rate = _core.ProfileRate()
            rate.segment, rate.ps, rate.hz, rate.record = segment, ps, value, records
            rates.append(rate)
            mirror.rate(value, ps)
        else:
            records += 1
            write = _core.ProfileWrite()
            write.segment, write.value, write.ps, write.record = segment, value, ps, records
            write.pc = 0x0800_0100 + 2 * records if pcs is None else pcs[records - 1]
            if keep_logs:
                logs[register].writes = [*logs[register].writes, write]
            if register == KR:
                mirror.key(write)
            else:
                mirror.change(register, write)
    clock = _core.ProfileClock()
    clock.segment, clock.ps = segment, end_ps - segment_start
    clocks.append(clock)
    for register, log in logs.items():
        log.id = register
    result.write_logs = list(logs.values())
    result.domain_rates = rates
    result.clocks = clocks
    result.reload_tracks = [mirror.finish()]
    return result


def setup(ps: int, pr: int, rlr: int) -> list[Event]:
    return [("w", KR, 0x5555, ps), ("w", PR, pr, ps + 1), ("w", RLR, rlr, ps + 2)]


def test_the_start_counts_from_0xfff_whatever_rlr_holds() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 100),
        ("w", KR, 0xCCCC, 10),
        ("w", KR, 0xAAAA, ms(20).ps),
    ]
    dog = watchdog(synthetic(events, ms(30).ps), REGISTERS)
    assert dog.starts == dog.refreshes == 1
    assert dog.min_margin_ps == 4 * 0xFFF * PERIOD_PS - (ms(20).ps - 10)
    assert dog.timeout_ps == 4 * 100 * PERIOD_PS


def test_after_a_cut_pr_and_rlr_are_unknown_until_written() -> None:
    cut = ms(100).ps
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 3, 500),
        ("w", KR, 0xAAAA, 10),
        ("cut", 0, 0, cut),
        ("r", 0, LSI_HZ, cut),
        ("w", KR, 0xAAAA, cut + ms(1).ps),
        ("w", KR, 0xAAAA, cut + ms(2).ps),
    ]
    dog = watchdog(synthetic(events, cut + ms(3).ps), REGISTERS)
    assert dog.refreshes == 3
    assert dog.timeout_ps is None
    assert dog.min_margin_ps is None
    events += [*setup(cut + ms(4).ps, 3, 500), ("w", KR, 0xAAAA, cut + ms(5).ps)]
    known = watchdog(synthetic(events, cut + ms(6).ps), REGISTERS)
    assert known.timeout_ps == ms(500).ps
    assert known.min_margin_ps is None


def test_a_rate_change_between_two_reloads_counts_each_part_at_its_rate() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 3, 500),
        ("w", KR, 0xAAAA, 0),
        ("r", 0, 250, ms(10).ps),
        ("w", KR, 0xAAAA, ms(100).ps),
    ]
    dog = watchdog(synthetic(events, ms(200).ps), REGISTERS)
    ticks_left = 32 * 500 - LSI_HZ * 10 // 1000
    expiry = ms(10).ps + ticks_left * 1_000_000_000_000 // 250
    assert dog.min_margin_ps == expiry - ms(100).ps
    assert dog.timeout_ps == 32 * 500 * 1_000_000_000_000 // 250


def test_a_reload_while_the_lsi_is_stopped_expires_from_when_it_runs() -> None:
    events: list[Event] = [
        ("r", 0, 0, 0),
        ("w", KR, 0xCCCC, 0),
        ("r", 0, LSI_HZ, ms(1).ps),
        ("w", KR, 0xAAAA, ms(2).ps),
    ]
    dog = watchdog(synthetic(events, ms(4).ps), REGISTERS)
    assert dog.min_margin_ps == ms(1).ps + 4 * 0xFFF * PERIOD_PS - ms(2).ps
    assert dog.timeout_ps == 4 * 0xFFF * PERIOD_PS
    stopped = watchdog(synthetic([*events, ("r", 0, 0, ms(3).ps)], ms(4).ps), REGISTERS)
    assert stopped.timeout_ps is None
    assert stopped.min_margin_ps is None


def test_pr_and_rlr_writes_the_hardware_ignores_leave_the_timeout() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 0x1F00 | 200),
        ("w", KR, 0xAAAA, 10),
        ("w", RLR, 50, 20),
        ("w", PR, 5, 21),
        ("w", KR, 0xAAAA, 30),
    ]
    dog = watchdog(synthetic(events, ms(1).ps), REGISTERS)
    assert dog.timeout_ps == 4 * (0x1FC8 & 0xFFF) * PERIOD_PS
    unknown: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        ("w", RLR, 50, 1),
        ("w", KR, 0xAAAA, 10),
    ]
    assert watchdog(synthetic(unknown, ms(1).ps), REGISTERS).timeout_ps is None


def test_writes_at_one_instruction_count_apply_in_stream_order() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        ("w", KR, 0x5555, 0),
        ("w", PR, 3, 0),
        ("w", KR, 0xAAAA, 0),
        ("w", PR, 7, 0),
        ("w", KR, 0xAAAA, ms(1).ps),
    ]
    dog = watchdog(synthetic(events, ms(2).ps), REGISTERS)
    assert dog.timeout_ps == 32 * 0xFFF * PERIOD_PS


def key_paths(value: object, prefix: str = "") -> set[str]:
    if isinstance(value, dict):
        items = cast(dict[str, object], value)
        return {
            path
            for key, item in items.items()
            for path in {f"{prefix}.{key}", *key_paths(item, f"{prefix}.{key}")}
        }
    if isinstance(value, list):
        return {
            path for item in cast(list[object], value) for path in key_paths(item, f"{prefix}[]")
        }
    return set()


def type_paths(kind: object, prefix: str) -> set[str]:
    origin = typing.get_origin(kind)
    if origin in (typing.Union, types.UnionType):
        inner = [arg for arg in typing.get_args(kind) if arg is not type(None)]
        return type_paths(inner[0], prefix)
    if origin is tuple:
        args = typing.get_args(kind)
        return type_paths(args[0], f"{prefix}[]") if args[1:] == (Ellipsis,) else set()
    if dataclasses.is_dataclass(kind) and isinstance(kind, type):
        hints = typing.get_type_hints(kind)
        return {
            path
            for field in dataclasses.fields(kind)
            for path in {
                f"{prefix}.{field.name}",
                *type_paths(hints[field.name], f"{prefix}.{field.name}"),
            }
        }
    return set()


def stopped_report() -> Report:
    m = booted(IWDG)
    with m.profile(*Analysis, milestones=["HAL_IWDG_Refresh", 0x0800_0002]) as profile:
        advance(m, ms(250))
        m.var("refresh_enabled").value = 0
        advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
        m.reset(popcorn.ResetKind.WATCHDOG)
        advance(m, ms(50))
    return profile.report()


def test_a_report_with_a_gap_a_cut_and_an_unreached_milestone_reads_back_equal() -> None:
    report = stopped_report()
    assert report.cuts == 1
    assert report.watchdog is not None and len(report.watchdog.gaps) == 1
    assert report.timeline is not None and report.timeline[1].time_ps is None
    back = Report.from_json(report.to_json())
    assert back == report
    assert back.watchdog is not None and back.watchdog.gaps == report.watchdog.gaps


def test_the_schema_holds_its_key_set() -> None:
    contract = {".schema", ".report", *type_paths(Report, ".report")}
    assert sorted(contract) == SCHEMA_2_KEYS
    data = json.loads(stopped_report().to_json())
    under_dicts = (".report.sleep.reasons.", ".report.loops.loops[].enders.")
    seen = {path for path in key_paths(data) if not path.startswith(under_dicts)}
    assert seen <= contract
    assert len(seen) > 80


def test_the_schema_refuses_a_version_of_another_type() -> None:
    data = json.loads(spi_report(ms(1)).to_json())
    for schema in (True, 2.0, "2"):
        with pytest.raises(ValueError, match="schema 2"):
            Report.from_json(json.dumps({"schema": schema, "report": data["report"]}))


def test_a_diff_row_holds_every_measure_of_its_row_type() -> None:
    import dataclasses
    import typing

    from popcorn.profiling._diff import _tables  # pyright: ignore[reportPrivateUsage]

    report = stopped_report()

    built = _tables(report)
    assert report.hotspots is not None and report.irq is not None and report.sleep is not None
    assert report.watchdog is not None and report.timeline is not None
    assert report.peripherals is not None
    samples: dict[str, tuple[object, str, set[str]]] = {
        "summary": (report, "report", set()),
        "functions": (report.hotspots.functions[0], report.hotspots.functions[0].name, set()),
        "exceptions": (report.irq.exceptions[0], report.irq.exceptions[0].name, set()),
        "registers": (report.peripherals.cpu[0], f"cpu:{report.peripherals.cpu[0].name}", set()),
        "sleep": (report.sleep, "sleep", {f"reason_{name}" for name in report.sleep.reasons}),
        "watchdog": (report.watchdog, "watchdog", {"gaps"}),
        "timeline": (report.timeline[0], report.timeline[0].name, set()),
    }
    for table, (row, key, extra) in samples.items():
        hints = typing.get_type_hints(type(row))
        measures = {
            field.name
            for field in dataclasses.fields(typing.cast(typing.Any, row))
            if hints[field.name] in MEASURES and field.name not in IDENTITY
        }
        assert measures
        assert set(built[table][key]) == measures | extra, table


SCHEMA_2_KEYS = [
    ".report",
    ".report.analyses",
    ".report.clocks",
    ".report.clocks[].at",
    ".report.clocks[].hz",
    ".report.clocks[].instructions",
    ".report.clocks[].ps",
    ".report.clocks[].segment",
    ".report.cuts",
    ".report.elapsed_ps",
    ".report.elapsed_ticks",
    ".report.executed",
    ".report.from_reset",
    ".report.hotspots",
    ".report.hotspots.calls",
    ".report.hotspots.deep_calls",
    ".report.hotspots.frames",
    ".report.hotspots.frames[].entries",
    ".report.hotspots.frames[].inclusive",
    ".report.hotspots.frames[].key",
    ".report.hotspots.frames[].kind",
    ".report.hotspots.frames[].name",
    ".report.hotspots.frames[].parent",
    ".report.hotspots.frames[].self",
    ".report.hotspots.functions",
    ".report.hotspots.functions[].calls",
    ".report.hotspots.functions[].inclusive",
    ".report.hotspots.functions[].name",
    ".report.hotspots.functions[].self",
    ".report.hotspots.functions[].start",
    ".report.hotspots.unmatched_returns",
    ".report.irq",
    ".report.irq.burst_window_ps",
    ".report.irq.exceptions",
    ".report.irq.exceptions[].burst",
    ".report.irq.exceptions[].cut",
    ".report.irq.exceptions[].entries",
    ".report.irq.exceptions[].longest_ps",
    ".report.irq.exceptions[].max_depth",
    ".report.irq.exceptions[].name",
    ".report.irq.exceptions[].number",
    ".report.irq.exceptions[].open",
    ".report.irq.exceptions[].per_ms",
    ".report.irq.exceptions[].preempted_by",
    ".report.irq.exceptions[].self_time_ps",
    ".report.irq.exceptions[].tail_chained",
    ".report.irq.exceptions[].time_ps",
    ".report.irq.unmatched_exits",
    ".report.loops",
    ".report.loops.loops",
    ".report.loops.loops[].enders",
    ".report.loops.loops[].head",
    ".report.loops.loops[].last",
    ".report.loops.loops[].location",
    ".report.loops.loops[].passes",
    ".report.loops.loops[].runs",
    ".report.loops.loops[].share",
    ".report.loops.loops[].time_ps",
    ".report.loops.min_passes",
    ".report.peripherals",
    ".report.peripherals.cpu",
    ".report.peripherals.cpu[].address",
    ".report.peripherals.cpu[].last_value",
    ".report.peripherals.cpu[].name",
    ".report.peripherals.cpu[].reads",
    ".report.peripherals.cpu[].same_writes",
    ".report.peripherals.cpu[].writes",
    ".report.peripherals.dma",
    ".report.peripherals.dma[].address",
    ".report.peripherals.dma[].last_value",
    ".report.peripherals.dma[].name",
    ".report.peripherals.dma[].reads",
    ".report.peripherals.dma[].same_writes",
    ".report.peripherals.dma[].writes",
    ".report.sleep",
    ".report.sleep.count",
    ".report.sleep.longest_ps",
    ".report.sleep.other_gaps_ps",
    ".report.sleep.partial",
    ".report.sleep.reasons",
    ".report.sleep.share",
    ".report.sleep.time_ps",
    ".report.sleep.wakes",
    ".report.sleep.wakes[].count",
    ".report.sleep.wakes[].name",
    ".report.sleep.wakes[].time_ps",
    ".report.start",
    ".report.timeline",
    ".report.timeline[].address",
    ".report.timeline[].name",
    ".report.timeline[].time_ps",
    ".report.watchdog",
    ".report.watchdog.closest_pc",
    ".report.watchdog.gaps",
    ".report.watchdog.gaps[].length_ps",
    ".report.watchdog.gaps[].pc",
    ".report.watchdog.gaps[].start_ps",
    ".report.watchdog.gaps[].timeout_ps",
    ".report.watchdog.key_writes",
    ".report.watchdog.logged_until_ps",
    ".report.watchdog.min_margin_ps",
    ".report.watchdog.refreshes",
    ".report.watchdog.starts",
    ".report.watchdog.timeout_ps",
    ".schema",
]
"""Every key path of a schema 2 report, `[]` for the items of a list."""

"""`Machine.profile` and the `popcorn.profiling` report, on the tim and spi examples."""

from __future__ import annotations

import asyncio
import contextlib
import dataclasses
import gc
import subprocess
import sys
import weakref
from collections.abc import Callable

import popcorn
import pytest
from popcorn import System, UartLink, _core
from popcorn.devices import At25sf
from popcorn.profiling import (
    ALL,
    BURST_WINDOW_PS,
    NOTES,
    Analysis,
    FrameKind,
    FunctionRow,
    Profile,
    Report,
)
from popcorn.profiling._symbols import Names
from popcorn.profiling._tables import hotspots, peripherals
from popcorn.units import ms, s, us

from .conftest import ROOT
from .linked import member

EXAMPLES = ROOT / "examples" / "stm32u575"
TIM = popcorn.MachineDescription(
    name="tim-ticks",
    part="STM32U575QGIx",
    images=[popcorn.Elf(EXAMPLES / "tim" / "prebuilt" / "tim_ticks.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)
SPI = popcorn.MachineDescription(
    name="spi-flash",
    part="STM32U575QGIx",
    images=[popcorn.Elf(EXAMPLES / "spi" / "prebuilt" / "spi_flash.elf")],
    boot=popcorn.Boot(vtor=0x800C008, until="main", budget=1_000_000),
    board={
        "SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, page_program=us(800))]),
    },
)
IDLE_HEAD = 0x0800_E00C


def booted(description: popcorn.MachineDescription) -> popcorn.Machine:
    m = description.machine()
    asyncio.run(description.boot_machine(m))
    return m


def function(report: Report, name: str) -> FunctionRow:
    assert report.hotspots is not None
    return next(row for row in report.hotspots.functions if row.name == name)


def raw(profile: Profile) -> _core.ProfileResult:
    return profile._recording.result  # pyright: ignore[reportPrivateUsage]


def stop_at(m: popcorn.Machine, symbol: str, count: int) -> None:
    seen = [0]

    def handler(call: popcorn.Call) -> popcorn.Return:
        seen[0] += 1
        return call.stop() if seen[0] == count else call.proceed()

    m.intercept(symbol, handler)


def test_the_tim_example_sleeps_between_its_timer_interrupts() -> None:
    m = booted(TIM)
    start, executed = m.time, m.executed
    with m.profile() as profile:
        asyncio.run(m.run(for_=s(3)))
    report = profile.report()
    assert report.elapsed_ticks == 12_000_000
    assert report.elapsed_ps == (m.time - start).ps == 3_000_000_000_000
    assert report.executed == m.executed - executed == 63_111
    assert not report.from_reset
    assert [(c.hz, c.instructions, c.ps) for c in report.clocks] == [
        (4_000_000, 12_000_000, 3_000_000_000_000)
    ]
    assert report.sleep is not None
    assert report.sleep.count == 3_001
    assert report.sleep.partial == 1
    assert report.sleep.reasons["wfi"] == 3_001
    assert report.sleep.time_ps == 11_936_889 * 250_000
    assert report.sleep.longest_ps == 3_979 * 250_000
    assert report.sleep.share == report.sleep.time_ps / report.elapsed_ps
    assert round(100 * report.sleep.share, 1) == 99.5
    assert report.sleep.other_gaps_ps == 0
    assert [(w.name, w.count, w.time_ps) for w in report.sleep.wakes] == [
        ("TIM2", 2_999, 2_983_240_500_000),
        ("TIM16", 1, 21 * 250_000),
    ]
    assert report.irq is not None
    tim2 = next(row for row in report.irq.exceptions if row.name == "TIM2")
    assert (tim2.number, tim2.entries, tim2.burst) == (16 + 45, 2_999, 1)
    assert (tim2.time_ps, tim2.self_time_ps, tim2.longest_ps) == (
        56_981 * 250_000,
        56_981 * 250_000,
        19 * 250_000,
    )
    assert tim2.per_ms == pytest.approx(2_999 / 3_000)
    assert report.irq.burst_window_ps == BURST_WINDOW_PS == 1_000_000_000
    assert function(report, "TIM2_IRQHandler").self == 56_981
    assert report.peripherals is not None
    sr = next(row for row in report.peripherals.cpu if row.name == "TIM2.SR")
    assert (sr.address, sr.reads, sr.writes, sr.same_writes) == (0x4000_0010, 2_999, 3_000, 2_999)
    assert report.loops is not None
    assert report.loops.loops == ()


def test_the_spi_example_spins_in_its_idle_task() -> None:
    m = booted(SPI)
    start, executed = m.time, m.executed
    with m.profile() as profile:
        asyncio.run(m.run(for_=ms(100)))
    report = profile.report()
    assert report.executed == m.executed - executed == 15_829_458
    assert report.elapsed_ps == (m.time - start).ps == 100_000_031_250
    assert [(c.hz, c.instructions, c.ps) for c in report.clocks] == [
        (4_000_000, 4_373, 1_093_250_000),
        (160_000_000, 15_825_085, 98_906_781_250),
    ]
    assert report.elapsed_ticks == 15_829_458
    assert report.hotspots is not None
    assert sum(row.self for row in report.hotspots.functions) == report.executed
    idle = function(report, "prvIdleTask")
    assert (idle.self, idle.inclusive, idle.calls) == (5_688_015, 15_665_295, 0)
    check = function(report, "prvCheckTasksWaitingTermination")
    assert (check.self, check.inclusive, check.calls) == (9_954_046, 9_968_787, 1_422_004)
    assert report.hotspots.functions[0] == check
    assert report.irq is not None
    rows = {row.name: (row.entries, row.burst) for row in report.irq.exceptions}
    assert (rows["SPI1"], rows["PendSV"], rows["SysTick"], rows["TIM6"]) == (
        (892, 892),
        (406, 212),
        (98, 2),
        (98, 2),
    )
    assert report.sleep is not None
    assert report.sleep.count == 0
    assert report.loops is not None
    idle_loop = report.loops.loops[0]
    assert idle_loop.head == IDLE_HEAD
    assert idle_loop.location == "prvCheckTasksWaitingTermination"
    assert (idle_loop.runs, idle_loop.passes, idle_loop.time_ps) == (197, 1_421_634, 97_746_750_000)
    assert idle_loop.enders == {"exception": 196, "end": 1}
    assert idle_loop.share == idle_loop.time_ps / report.elapsed_ps
    native_loop = next(loop for loop in raw(profile).loops if loop.head == IDLE_HEAD)
    assert native_loop.instructions == 15_639_480


def test_a_loop_request_of_one_pass_counts_the_shorter_runs_too() -> None:
    m = booted(SPI)
    with m._profile(loop_reports=1) as recording:  # pyright: ignore[reportPrivateUsage]
        asyncio.run(m.run(for_=ms(100)))
    loop = next(loop for loop in recording.result.loops if loop.head == IDLE_HEAD)
    assert (loop.runs, loop.passes, loop.instructions) == (215, 1_421_677, 15_640_093)
    assert recording.result.instructions == 15_829_458


def test_the_hotspots_match_an_independent_coverage_run_of_the_same_scenario() -> None:
    covered = booted(SPI)
    covered.coverage.enable()
    executed = covered.executed
    asyncio.run(covered.run(for_=ms(20)))
    hits: dict[int, int] = {}
    for group in covered.coverage.collect().groups:
        for block in group.blocks:
            hits[block.start] = hits.get(block.start, 0) + block.hits
    profiled = booted(SPI)
    with profiled.profile("hotspots") as profile:
        asyncio.run(profiled.run(for_=ms(20)))
    assert profile.report().executed == covered.executed - executed
    assert {block.pc: block.entries for block in raw(profile).code} == hits


def test_a_profile_leaves_the_user_coverage_as_a_run_without_it() -> None:
    def covered(with_profile: bool) -> object:
        m = booted(SPI)
        m.coverage.enable(functions=["spi_exchange"])
        with contextlib.ExitStack() as stack:
            if with_profile:
                stack.enter_context(m.profile())
            asyncio.run(m.run(for_=ms(10)))
        return m.coverage.collect()

    assert covered(True) == covered(False)


def stopped_profile(running: bool) -> Report:
    m = booted(SPI)
    stop_at(m, "TIM6_IRQHandler", 40)
    stack = contextlib.ExitStack()
    profile = stack.enter_context(m.profile())
    if not running:
        stop = asyncio.run(m.run(for_=ms(500)))
        assert stop.reason is popcorn.StopReason.USER
        stack.close()
        return profile.report()

    async def body() -> None:
        async with m.running():
            with pytest.raises(popcorn.EmulationError):
                await m.wait_until(lambda _: False)
            stack.close()
            await profile.wait_closed()

    with pytest.raises(popcorn.EmulationError):
        asyncio.run(body())
    return profile.report()


def test_a_profile_reports_the_same_parked_and_inside_running() -> None:
    parked = stopped_profile(running=False)
    assert parked.loops is not None
    assert parked.loops.loops
    assert dataclasses.asdict(stopped_profile(running=True)) == dataclasses.asdict(parked)


def test_debugger_reads_during_a_profile_leave_the_cpu_table_unchanged() -> None:
    def table(read: bool) -> tuple[object, int]:
        m = booted(SPI)
        spi1 = next(p for p in m.description.peripherals if p.name == "SPI1")
        sr = spi1.base + spi1.register("SR").offset
        with m.profile("peripherals") as profile:
            for _ in range(20):
                asyncio.run(m.run(for_=us(500)))
                if read:
                    m.mem.read(sr, 4)
        report = profile.report()
        assert report.peripherals is not None
        return report.peripherals.cpu, len(raw(profile).debugger_registers)

    cpu, debugged = table(True)
    assert debugged == 1
    assert (cpu, 0) == table(False)


def test_a_reset_inside_a_profile_cuts_and_keeps_every_total_counted() -> None:
    m = booted(TIM)
    with m.profile() as profile:
        asyncio.run(m.run(for_=ms(5)))
        m.reset()
        asyncio.run(m.run(for_=ms(5)))
    report = profile.report()
    assert report.cuts == 1
    assert [c.segment for c in report.clocks] == [0, 1]
    assert sum(c.instructions for c in report.clocks) == report.elapsed_ticks
    assert report.elapsed_ps == sum(c.ps for c in report.clocks)
    assert report.irq is not None
    assert all(row.time_ps >= row.self_time_ps >= 0 for row in report.irq.exceptions)
    assert report.sleep is not None
    assert 0 < report.sleep.time_ps < report.elapsed_ps
    assert report.sleep.time_ps == raw(profile).sleeps.total * 250_000


def test_a_profile_opened_at_the_reset_holds_the_boot() -> None:
    m = TIM.machine()
    m.reset()
    with m.profile("hotspots") as profile:
        asyncio.run(m.run(until="main", budget=1_000_000))
    report = profile.report()
    assert report.from_reset
    assert report.start == 0
    assert function(report, "Reset_Handler").self > 0


def test_analyses_choose_the_sections_and_the_sink_options() -> None:
    m = booted(TIM)
    with m.profile("sleep", Analysis.PERIPHERALS, "sleep") as profile:
        asyncio.run(m.run(for_=ms(5)))
    report = profile.report()
    assert report.analyses == (Analysis.SLEEP, Analysis.PERIPHERALS)
    assert (report.hotspots, report.irq, report.loops) == (None, None, None)
    assert report.sleep is not None and report.peripherals is not None
    assert not raw(profile).frames
    assert raw(profile).exceptions[16 + 45].burst == 0
    with m.profile() as everything:
        pass
    assert everything.analyses == ALL
    assert everything.report().analyses == ALL


def test_an_unknown_analysis_is_refused_before_anything_changes() -> None:
    m = booted(TIM)
    with pytest.raises(ValueError, match="no analysis 'heat'"), m.profile("sleep", "heat"):
        pass
    assert m._handle.loop_reports() == 0  # pyright: ignore[reportPrivateUsage]
    with m.profile("loops"):
        assert m._handle.loop_reports() == 4  # pyright: ignore[reportPrivateUsage]


def test_the_report_is_refused_while_the_profile_is_open() -> None:
    m = booted(TIM)
    with m.profile() as profile:
        assert not profile.closed
        with pytest.raises(popcorn.MachineRunning):
            profile.report()
    assert profile.closed
    assert profile.report() is profile.report()


def test_a_profile_refuses_a_member_of_a_linked_system(u575: popcorn.PartDescription) -> None:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    system = System(
        {"a": a, "b": b}, [UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)]
    )
    try:
        with pytest.raises(popcorn.ConfigError), a.profile():
            pass
    finally:
        system.close()


def test_a_kept_profile_keeps_no_machine_alive() -> None:
    m = booted(TIM)
    alive = weakref.ref(m)
    with m.profile() as profile:
        asyncio.run(m.run(for_=ms(2)))
    del m
    gc.collect()
    assert alive() is None
    assert profile.report().executed > 0


def test_the_text_holds_every_section_asked_for_and_the_notes() -> None:
    m = booted(SPI)
    with m.profile() as profile:
        asyncio.run(m.run(for_=ms(5)))
    text = profile.report().text()
    for heading in ("hotspots", "sleep", "irq", "peripherals", "loops"):
        assert f"== {heading}" in text
    assert "prvCheckTasksWaitingTermination" in text
    assert "no boot in it" in text
    assert all(note in text for note in NOTES)
    with m.profile("irq") as only:
        pass
    assert "== hotspots" not in only.report().text()


def frame(
    parent: int, kind: str, key: int, entries: int, own: int, total: int
) -> _core.ProfileFrame:
    node = _core.ProfileFrame()
    node.parent = parent
    node.kind = getattr(_core.ProfileFrameKind, kind)
    node.key = key
    node.entries = entries
    node.self = own
    node.inclusive = total
    return node


def block(pc: int, entries: int, instructions: int) -> _core.ProfileBlock:
    row = _core.ProfileBlock()
    row.pc = pc
    row.entries = entries
    row.instructions = instructions
    return row


def test_inclusive_time_counts_the_outermost_frame_of_a_recursive_function_once() -> None:
    m = booted(TIM)
    names = Names(m.images, m.description)
    main = m.sym("main") & ~1
    handler = m.sym("TIM2_IRQHandler") & ~1
    result = _core.ProfileResult()
    result.code = [block(main, 3, 30), block(handler, 2, 8)]
    result.instructions = 38
    result.frames = [
        frame(0, "root", 0, 0, 0, 38),
        frame(0, "entry", main, 1, 10, 38),
        frame(1, "call", main, 1, 10, 28),
        frame(2, "exception", 16 + 45, 1, 0, 8),
        frame(3, "entry", handler, 1, 4, 8),
        frame(4, "call", main, 1, 4, 4),
        frame(2, "call", main, 2, 10, 10),
    ]
    rows = {row.name: row for row in hotspots(result, names).functions}
    assert (rows["main"].self, rows["main"].inclusive, rows["main"].calls) == (30, 38, 4)
    assert (rows["TIM2_IRQHandler"].inclusive, rows["TIM2_IRQHandler"].calls) == (8, 0)
    tree = hotspots(result, names).frames
    assert [(node.parent, node.kind, node.key) for node in tree] == [
        (-1, FrameKind.ROOT, 0),
        (0, FrameKind.ENTRY, main),
        (1, FrameKind.CALL, main),
        (2, FrameKind.EXCEPTION, 16 + 45),
        (3, FrameKind.ENTRY, handler),
        (4, FrameKind.CALL, main),
        (2, FrameKind.CALL, main),
    ]
    assert [node.name for node in tree] == [
        "",
        "main",
        "main",
        "TIM2",
        "TIM2_IRQHandler",
        "main",
        "main",
    ]


def test_profiling_is_imported_only_by_a_profile() -> None:
    code = (
        "import sys, popcorn, popcorn.pytest;"
        "assert 'popcorn.profiling' not in sys.modules;"
        "print('ok')"
    )
    out = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, timeout=60, check=False
    )
    assert out.stdout.strip() == "ok", out.stderr


SECTIONS: dict[Analysis, Callable[[Report], object]] = {
    Analysis.HOTSPOTS: lambda r: r.hotspots,
    Analysis.SLEEP: lambda r: r.sleep,
    Analysis.IRQ: lambda r: r.irq,
    Analysis.PERIPHERALS: lambda r: r.peripherals,
    Analysis.LOOPS: lambda r: r.loops,
}


@pytest.mark.parametrize("analysis", ALL)
def test_each_analysis_alone_fills_its_own_section(analysis: Analysis) -> None:
    m = booted(TIM)
    with m.profile(analysis) as profile:
        asyncio.run(m.run(for_=ms(2)))
    report = profile.report()
    assert [a for a in ALL if SECTIONS[a](report) is not None] == [analysis]


def register_address(m: popcorn.Machine, peripheral: str, register: str) -> int:
    found = next(p for p in m.description.peripherals if p.name == peripheral)
    return found.base + found.register(register).offset


def write_word(m: popcorn.Machine, at: int, update: Callable[[int], int]) -> None:
    word = int.from_bytes(m.mem.read(at, 4), "little")
    m.mem.write(at, update(word).to_bytes(4, "little"))


def test_time_shares_are_picoseconds_across_a_core_rate_change() -> None:
    m = booted(TIM)
    start = m.time
    with m.profile("sleep", "irq") as whole:
        with m.profile("sleep") as slow:
            asyncio.run(m.run(for_=ms(20)))
        switched = m.time
        write_word(m, register_address(m, "FLASH", "ACR"), lambda word: (word & ~0xF) | 1)
        write_word(
            m,
            register_address(m, "RCC", "ICSCR1"),
            lambda word: (word & 0x0FFF_FFFF) | 2 << 28 | 1 << 23,
        )
        with m.profile("sleep") as fast:
            asyncio.run(m.run(for_=ms(20)))
    report = whole.report()
    assert [(c.hz, c.ps) for c in report.clocks] == [
        (4_000_000, (switched - start).ps),
        (16_000_000, (m.time - switched).ps),
    ]
    assert report.elapsed_ps == (m.time - start).ps
    slow_ticks = raw(slow).sleeps.total
    fast_ticks = raw(fast).sleeps.total
    asleep_ps = slow_ticks * 250_000 + fast_ticks * 62_500
    assert report.sleep is not None
    assert report.sleep.time_ps == asleep_ps
    assert report.sleep.share == asleep_ps / (m.time - start).ps
    assert report.sleep.share != raw(whole).sleeps.total / report.elapsed_ticks
    assert report.irq is not None
    tim2 = next(row for row in report.irq.exceptions if row.name == "TIM2")
    assert tim2.burst == 4


def test_aliased_functions_take_the_first_name_and_count_the_others() -> None:
    m = booted(SPI)
    names = Names(m.images, m.description)
    handler = m.sym("WWDG_IRQHandler") & ~1
    aliases = sorted(
        symbol.name
        for image in m.images
        for symbol in image.symbols
        if symbol.function and symbol.value & ~1 == handler
    )
    assert len(aliases) > 2
    name, start = names.function(handler)
    assert start == handler
    assert name == f"{aliases[0]} (+{len(aliases) - 1} aliases)"
    assert names.function(m.sym("SPI1_IRQHandler") & ~1)[0] == "SPI1_IRQHandler"


def test_a_register_row_takes_its_address_from_the_description_when_known() -> None:
    m = booted(TIM)
    names = Names(m.images, m.description)
    sr = register_address(m, "TIM2", "SR")
    flat = next(
        index
        for index, (peripheral, register) in enumerate(
            (p, r) for p in m.description.peripherals for r in p.registers
        )
        if (peripheral.name, register.name) == ("TIM2", "SR")
    )
    known = _core.ProfileRegister()
    known.id = flat
    known.first_address = sr + 2
    known.writes = 2
    unknown = _core.ProfileRegister()
    unknown.id = 0xFFFF
    unknown.first_address = 0x5FFF_0001
    unknown.reads = 1
    result = _core.ProfileResult()
    result.cpu_registers = [known, unknown]
    rows = {row.name: row.address for row in peripherals(result, names).cpu}
    assert rows == {"TIM2.SR": sr, "0x5fff0001": 0x5FFF_0001}

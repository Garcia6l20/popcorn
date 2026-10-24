"""Coverage, full traces, the trace ring and log capture from a Machine."""

from __future__ import annotations

import asyncio
import gc
import logging
import weakref
from pathlib import Path

import popcorn
import pytest
from popcorn import _obs
from popcorn._observe import collapsed
from popcorn.coverage import Coverage, ImageKind
from popcorn.trace import ALL, Block, Mmio, Stop, TraceKind, read
from popcorn.units import ms

from .conftest import CORE_FIRMWARE, M33_JSON, ROOT, load_each, run_bounded
from .models_example import Timer

BRANCHES_ELF = CORE_FIRMWARE / "branches.elf"
MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"
MODEL_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "model.elf"
FAULT_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "fault.elf"


def branches(count: int = 4) -> popcorn.Machine:
    """A machine at `branches` with r0 = count, coverage on over the function."""
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(BRANCHES_ELF))
    m.reset()
    asyncio.run(m.run(budget=1_000))
    m.coverage.enable(functions=["branches"])
    m.reg.pc = m.sym("branches") & ~1
    m.reg.r0 = count
    return m


def run(m: popcorn.Machine) -> popcorn.Stop:
    return asyncio.run(m.run(budget=10_000))


def hits_by_symbol(m: popcorn.Machine, coverage: Coverage) -> dict[str, int]:
    names = {s.value & ~1: s.name for s in m.images[0].symbols if s.name.startswith("branch")}
    return {names[b.start]: b.hits for b in coverage.groups[0].blocks if b.start in names}


def test_coverage_counts_the_blocks_of_a_function() -> None:
    m = branches()
    assert m.coverage.enabled
    run(m)
    coverage = m.coverage.collect()
    group = coverage.groups[0]
    assert [(r.name, r.size) for r in group.ranges] == [("branches", 30)]
    assert hits_by_symbol(m, coverage) == {
        "branches": 1,
        "branch_top": 3,
        "branch_odd": 2,
        "branch_even": 2,
        "branch_join": 4,
        "branch_end": 1,
    }
    image = group.images[0]
    assert image.kind is ImageKind.ELF
    assert Path(image.path) == BRANCHES_ELF.resolve()
    assert image.digest == m.images[0].digest


def test_coverage_saves_merges_disables_and_clears(tmp_path: Path) -> None:
    m = branches()
    run(m)
    saved = m.coverage.save(tmp_path / "w0.pcov")
    assert Coverage.load(tmp_path / "w0.pcov") == saved
    doubled = m.coverage.merge_into(tmp_path / "w0.pcov")
    assert hits_by_symbol(m, doubled)["branch_join"] == 8
    m.coverage.disable()
    assert not m.coverage.enabled
    m.reg.pc = m.sym("branches") & ~1
    m.reg.r0 = 4
    run(m)
    assert m.coverage.collect() == saved
    m.coverage.clear()
    assert hits_by_symbol(m, m.coverage.collect()) == {}


def test_coverage_of_a_raw_image_has_no_path(tmp_path: Path) -> None:
    blob = tmp_path / "blob.bin"
    blob.write_bytes(bytes(range(16)))
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(BRANCHES_ELF))
    m.load(popcorn.Image.bin(blob, 0x2000_0100))
    images = m.coverage.collect().groups[0].images
    assert [(i.kind, i.path) for i in images] == [
        (ImageKind.ELF, str(BRANCHES_ELF.resolve())),
        (ImageKind.RAW, ""),
    ]
    assert not m.images[1].is_elf and m.images[0].is_elf


def test_coverage_ranges_are_checked() -> None:
    m = branches()
    with pytest.raises(popcorn.ConfigError):
        m.coverage.enable(ranges=[(0x0800_0000, 0)])
    with pytest.raises(popcorn.ConfigError):
        m.coverage.enable(ranges=[(0xFFFF_FF00, 0x200)])
    with pytest.raises(popcorn.SymbolError):
        m.coverage.enable(functions=["no_such_function"])
    m.coverage.enable(ranges=[(m.sym("branch_top") & ~1, 8)])
    run(m)
    names = {r.name for r in m.coverage.collect().groups[0].ranges}
    assert names == {"branches", f"{m.sym('branch_top') & ~1:#010x}"}


def test_enabling_nothing_counts_nothing() -> None:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(BRANCHES_ELF))
    m.coverage.enable(ranges=[], functions=[])
    assert not m.coverage.enabled
    m.coverage.enable()
    assert m.coverage.enabled
    assert [r.name for r in m.coverage.collect().groups[0].ranges] == [m.images[0].segments[0].name]


def test_a_trace_writes_blocks_to_a_file(tmp_path: Path) -> None:
    m = branches()
    path = tmp_path / "t.pctr"
    with m.trace({TraceKind.BLOCK}, path) as recording:
        run(m)
    assert recording.size == path.stat().st_size
    blocks = [r for r in read(path) if isinstance(r, Block)]
    assert blocks[0].pc == m.sym("branches") & ~1
    assert len(blocks) == recording.records - 1
    m.reg.pc = m.sym("branches") & ~1
    m.reg.r0 = 2
    assert run(m).reason is popcorn.StopReason.USER
    assert path.stat().st_size == recording.size


def modelled(name: str | None = None) -> popcorn.Machine:
    m = popcorn.Machine(
        popcorn.load_description(MODEL_JSON),
        image=popcorn.Image.elf(MODEL_ELF),
        models={"TIMER": Timer},
        name=name,
    )
    m.reset()
    asyncio.run(m.run(until=m.sym("ready"), budget=10_000))
    return m


def test_a_trace_names_mmio_registers(tmp_path: Path) -> None:
    m = modelled()
    path = tmp_path / "mmio.pctr"
    with m.trace({TraceKind.BLOCK, TraceKind.MMIO}, path):
        asyncio.run(m.call("start_timer", 10))
    records = list(read(path))
    mmio = [r for r in records if isinstance(r, Mmio)]
    assert [(r.register, r.value, r.write) for r in mmio] == [
        ("TIMER.ARR", 10, True),
        ("TIMER.CR", 1, True),
    ]
    assert any(isinstance(r, Block) for r in records)


def test_the_ring_dump_names_registers_and_the_stop() -> None:
    m = modelled()
    result = asyncio.run(m.call("start_timer", 10))
    records = m.ring.records()
    stops = [r for r in records if isinstance(r, Stop)]
    assert stops[-1].time == result.stop.instructions
    assert any(isinstance(r, Mmio) and r.register == "TIMER.CR" for r in records)
    assert any(isinstance(r, Stop) for r in records)
    dump = m.ring.dump()
    assert "TIMER.CR" in dump
    assert "stop  ADDRESS at" in dump or "stop  RETURNED at" in dump


def test_a_collapsed_dump_prints_a_repeated_call_once() -> None:
    m = modelled()
    m.ring.clear()
    for _ in range(5):
        asyncio.run(m.call("start_timer", 10))
    full = m.ring.dump().splitlines()
    short = m.ring.dump(collapse=True).splitlines()
    assert len(full) == 15
    assert short[:3] == full[:3]
    assert (
        short[3] == f"{'':>12} ... 3 records repeated 4 more times, last at {full[-1].split()[0]}"
    )
    assert len(short) == 4


def test_a_dump_keeps_its_last_lines() -> None:
    m = modelled()
    asyncio.run(m.call("start_timer", 10))
    full = m.ring.dump().splitlines()
    assert len(full) > 2
    last = m.ring.dump(last=2).splitlines()
    assert last == [f"... {len(full) - 2} earlier lines", *full[-2:]]
    assert m.ring.dump(last=len(full)).splitlines() == full


def test_a_dump_refuses_a_last_below_one() -> None:
    m = modelled()
    with pytest.raises(ValueError, match="last must be at least 1, not 0"):
        m.ring.dump(last=0)


def nested(*, blocks: bool) -> popcorn.Machine:
    """A machine of `fault.elf` reset into `vt_nested`, its ring keeping BLOCK records when
    `blocks`."""
    image = popcorn.Image.elf(FAULT_ELF)
    m = popcorn.Machine(M33_JSON, image=image, vector_table=image.lookup("vt_nested").value)
    m.ring.blocks = blocks
    m.reset()
    return m


def test_ring_blocks_lead_to_the_fault() -> None:
    m = nested(blocks=True)
    assert m.ring.blocks
    stop = asyncio.run(m.run(budget=10_000))
    assert stop.reason is popcorn.StopReason.FAULT
    records = m.ring.records()
    assert isinstance(records[-1], Stop)
    assert isinstance(records[-2], Block)
    blocks = [r for r in records if isinstance(r, Block)]
    assert blocks[-1].pc <= stop.pc < blocks[-1].pc + 2 * blocks[-1].icount
    assert sum(b.icount for b in blocks) == stop.instructions
    assert "block " in m.ring.dump()


def test_the_ring_keeps_no_block_by_default() -> None:
    m = nested(blocks=False)
    assert not m.ring.blocks
    assert asyncio.run(m.run(budget=10_000)).reason is popcorn.StopReason.FAULT
    assert not any(isinstance(r, Block) for r in m.ring.records())


def ring_block_rows(m: popcorn.Machine) -> list[tuple[int, int, int]]:
    return [(r.time, r.pc, r.icount) for r in m.ring.records() if isinstance(r, Block)]


@pytest.mark.timeout(40)
def test_ring_blocks_are_alike_parked_and_running() -> None:
    parked = nested(blocks=True)
    stop = asyncio.run(parked.run(budget=10_000))
    assert stop.reason is popcorn.StopReason.FAULT
    running = nested(blocks=True)

    async def body() -> None:
        async with running.running():
            await running.wait_for(popcorn.Reached("level3"))

    with pytest.raises(popcorn.FaultError) as raised:
        run_bounded(body(), 30)
    ended = raised.value.stop
    assert ended is not None and ended.pc == stop.pc
    assert ring_block_rows(parked)
    assert ring_block_rows(running) == ring_block_rows(parked)


def test_ring_blocks_are_checked_and_refused_while_running() -> None:
    m = branches(count=1_000_000)
    with pytest.raises(TypeError, match=r"Ring\.blocks takes a bool, not int"):
        m.ring.blocks = 1  # pyright: ignore[reportAttributeAccessIssue]

    async def body() -> None:
        async with m.running():
            with pytest.raises(TypeError, match="not int"):
                m.ring.blocks = 1  # pyright: ignore[reportAttributeAccessIssue]
            with pytest.raises(popcorn.MachineRunning):
                m.ring.blocks = True

    asyncio.run(body())
    assert not m.ring.blocks


@pytest.mark.timeout(40)
def test_ring_blocks_are_refused_from_a_command_of_a_free_run() -> None:
    m = branches(count=1_000_000)
    refused: list[type[BaseException]] = []

    def toggle() -> None:
        try:
            m.ring.blocks = True
        except popcorn.MachineRunning as error:
            refused.append(type(error))

    async def body() -> None:
        async with m.running():
            await m.sample(toggle)

    run_bounded(body(), 30)
    assert refused == [popcorn.MachineRunning]
    assert not m.ring.blocks


@pytest.mark.timeout(40)
def test_ring_blocks_are_refused_from_a_handler_in_a_slice() -> None:
    m = branches(count=2)
    refused: list[type[BaseException]] = []

    def handler(call: popcorn.Call) -> popcorn.Return:
        try:
            m.ring.blocks = True
        except popcorn.MachineRunning as error:
            refused.append(type(error))
        return call.proceed()

    m.intercept("branches", handler)

    async def body() -> None:
        async with m.running():
            await m.wait_for(popcorn.Reached("outside"))

    with pytest.raises(popcorn.EmulationError):
        run_bounded(body(), 30)
    assert refused == [popcorn.MachineRunning]
    assert not m.ring.blocks


def test_ring_blocks_stay_out_of_snapshots() -> None:
    m = branches()
    m.ring.blocks = True
    taken = m.snapshot()
    m.ring.blocks = False
    m.restore(taken)
    assert not m.ring.blocks
    m.ring.blocks = True
    m.restore(taken)
    assert m.ring.blocks


def test_collapse_finds_a_single_repeated_record() -> None:
    lines = [(t, "mmio  write32 IWDG.KR = 0xaaaa at f") for t in range(10, 15)]
    assert collapsed(lines) == [
        f"{10:>12} mmio  write32 IWDG.KR = 0xaaaa at f",
        f"{'':>12} ... record repeated 4 more times, last at 14",
    ]


def test_collapse_finds_an_alternating_loop_and_keeps_the_rest() -> None:
    loop = ["block loop 3 insn", "mmio  write32 IWDG.KR = 0xaaaa at loop"]
    lines = [(0, "call  main r0=0x0")]
    lines += [(1 + i, loop[i % 2]) for i in range(6)]
    lines += [(7, "stop  BUDGET at loop")]
    assert collapsed(lines) == [
        f"{0:>12} call  main r0=0x0",
        f"{1:>12} {loop[0]}",
        f"{2:>12} {loop[1]}",
        f"{'':>12} ... 2 records repeated 2 more times, last at 5",
        f"{7:>12} stop  BUDGET at loop",
    ]


def test_collapse_leaves_distinct_records_alone() -> None:
    lines = [(t, f"irq   enter {t}") for t in range(4)]
    assert collapsed(lines) == [f"{t:>12} irq   enter {t}" for t in range(4)]
    assert collapsed([]) == []


def test_a_wrapped_ring_keeps_absolute_times() -> None:
    m = popcorn.Machine(M33_JSON, image=popcorn.Image.elf(BRANCHES_ELF), trace_capacity=2)
    m.reset()
    stops: list[popcorn.Stop] = []
    for _ in range(3):
        m.reg.pc = m.sym("branches") & ~1
        m.reg.r0 = 3
        stops.append(run(m))
    records = m.ring.records()
    assert len(records) == 2
    assert [r.time for r in records if isinstance(r, Stop)] == [s.instructions for s in stops[1:]]


def test_an_unmodelled_access_is_logged_once(caplog: pytest.LogCaptureFixture) -> None:
    m = modelled()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(m.call("touch_unmodelled"))
        asyncio.run(m.call("touch_unmodelled"))
    logged = [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "UNMODELLED"]
    assert len(logged) == 1
    fields = vars(logged[0])
    assert (fields["popcorn_register"], fields["popcorn_offset"]) == ("DATA", 0)
    assert fields["popcorn_kind"] == "unmodelled_access"
    assert fields["popcorn_write"] is False
    touch = m.symbols["touch_unmodelled"]
    assert touch.value & ~1 <= fields["popcorn_pc"] < (touch.value & ~1) + touch.size
    assert logged[0].getMessage() == (
        "POPCORN-M33-TEST: read of UNMODELLED.DATA at offset 0x0, no model"
    )
    assert fields["popcorn_machine"] == "POPCORN-M33-TEST"


def unmodelled_records(
    caplog: pytest.LogCaptureFixture, machines: list[popcorn.Machine]
) -> list[logging.LogRecord]:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        for m in machines:
            asyncio.run(m.call("touch_unmodelled"))
    return [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "UNMODELLED"]


def test_a_named_machine_logs_under_its_name(caplog: pytest.LogCaptureFixture) -> None:
    (record,) = unmodelled_records(caplog, [modelled("node1")])
    assert record.getMessage() == "node1: read of UNMODELLED.DATA at offset 0x0, no model"
    assert vars(record)["popcorn_machine"] == "node1"


def test_two_machines_of_one_part_log_apart(caplog: pytest.LogCaptureFixture) -> None:
    records = unmodelled_records(caplog, [modelled("a"), modelled("b")])
    assert [r.getMessage().split(":")[0] for r in records] == ["a", "b"]
    assert [vars(r)["popcorn_machine"] for r in records] == ["a", "b"]


def test_a_renamed_machine_logs_and_raises_under_its_new_name(
    caplog: pytest.LogCaptureFixture,
) -> None:
    m = modelled("old")
    timer = next(p.base for p in m.description.peripherals if p.name == "TIMER")
    (before,) = unmodelled_records(caplog, [m])
    m._rename("new")  # pyright: ignore[reportPrivateUsage]
    assert m.name == "new"
    assert repr(m) == "Machine(new, POPCORN-M33-TEST, images=1)"
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(m.run(budget=load_each(m, [timer + 0x10])))
    hole = [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "TIMER"]
    assert [vars(r)["popcorn_machine"] for r in hole] == ["new"]
    assert vars(before)["popcorn_machine"] == "old"
    assert before.getMessage().startswith("old: read of UNMODELLED")
    assert hole[0].getMessage().startswith("new: read of TIMER")
    m.reg.pc = 0x3000_0000
    with pytest.raises(popcorn.FaultError, match=r"^new: the run letting time pass stopped"):
        asyncio.run(m.elapse(ms(1)))
    m._rename(m.description.part)  # pyright: ignore[reportPrivateUsage]
    assert repr(m) == "Machine(POPCORN-M33-TEST, images=1)"


def test_a_renamed_machine_is_collected() -> None:
    m = modelled("old")
    m._rename("new")  # pyright: ignore[reportPrivateUsage]
    probe = weakref.ref(m)
    del m
    gc.collect()
    assert probe() is None


def test_concurrent_machines_keep_their_records_apart(caplog: pytest.LogCaptureFixture) -> None:
    machines = [modelled("a"), modelled("b")]
    base = next(p.base for p in machines[0].description.peripherals if p.name == "TIMER")
    offsets = range(0x10, 0x10 + 4 * 50, 4)

    budgets = [load_each(m, [base + offset for offset in offsets]) for m in machines]

    async def body() -> None:
        await asyncio.gather(*(m.run(budget=b) for m, b in zip(machines, budgets, strict=True)))

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(body())
    for m in machines:
        mine = [r for r in caplog.records if vars(r).get("popcorn_machine") == m.name]
        assert [vars(r)["popcorn_offset"] for r in mine] == list(offsets)
        assert all(r.getMessage().startswith(f"{m.name}: read of TIMER") for r in mine)


def test_closing_a_trace_on_a_full_disk_raises() -> None:
    m = branches()
    with pytest.raises(_obs.FormatError), m.trace(ALL, "/dev/full"):
        run(m)


def test_trace_kinds_and_ranges_are_checked(tmp_path: Path) -> None:
    m = branches()
    with pytest.raises(popcorn.ConfigError), m.trace({TraceKind.STR}, tmp_path / "x.pctr"):
        pass
    with pytest.raises(popcorn.ConfigError), m.trace({TraceKind.TIME}, tmp_path / "t.pctr"):
        pass
    with pytest.raises(popcorn.ConfigError), m.trace(ALL, tmp_path / "y.pctr", ranges=[(0, 0)]):
        pass


def test_coverage_and_traces_refuse_a_running_machine(tmp_path: Path) -> None:
    m = branches(count=1_000_000)

    async def body() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning):
                m.coverage.clear()
            with pytest.raises(popcorn.MachineRunning):
                m.coverage.enable()
            with pytest.raises(popcorn.MachineRunning):
                m.coverage.collect()
            with pytest.raises(popcorn.MachineRunning), m.trace(ALL, tmp_path / "z.pctr"):
                pass
            with pytest.raises(popcorn.MachineRunning):
                m.ring.dump()

    asyncio.run(body())


def test_a_failing_body_keeps_its_error_and_notes_the_close_error() -> None:
    m = branches()
    with pytest.raises(LookupError) as caught, m.trace(ALL, "/dev/full"):
        run(m)
        raise LookupError("body failed")
    notes = getattr(caught.value, "__notes__", [])
    assert len(notes) == 1 and notes[0].startswith("closing the trace /dev/full also failed")


def test_a_trace_limited_to_a_function_records_only_its_blocks(tmp_path: Path) -> None:
    m = branches()
    start = m.symbols["branch_top"].value & ~1
    path = tmp_path / "top.pctr"
    with m.trace({TraceKind.BLOCK}, path, functions=["branch_top"]):
        run(m)
    pcs = [r.pc for r in read(path) if isinstance(r, Block)]
    assert pcs == [start] * 3


def test_a_trace_closes_after_its_machine_is_gone(tmp_path: Path) -> None:
    m = branches()
    path = tmp_path / "late.pctr"
    context = m.trace({TraceKind.BLOCK}, path)
    recording = context.__enter__()
    run(m)
    del m
    gc.collect()
    recording.close()
    assert recording.size == path.stat().st_size
    assert any(isinstance(r, Block) for r in read(path))


def test_a_machine_with_its_log_sink_is_freed() -> None:
    m = modelled()
    asyncio.run(m.call("touch_unmodelled"))
    ref = weakref.ref(m)
    del m
    gc.collect()
    assert ref() is None

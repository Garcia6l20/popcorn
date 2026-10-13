"""Intercepts, scripted replies, the intercept log, return hooks and libc stubs."""

from __future__ import annotations

import asyncio
import gc
import traceback
import weakref

import popcorn
import pytest
from popcorn.units import VirtualTime, ms

from .conftest import CALLEE_ELF, ISR_ELF, run_bounded


def call(m: popcorn.Machine, fn: str, *args: int) -> popcorn.CallResult:
    return asyncio.run(m.call(fn, *args, budget=1_000_000))


def test_an_intercept_returning_skips_the_body(hooked: popcorn.Machine) -> None:
    spins = hooked.sym("delay_spins")
    start = hooked.instructions
    slow = call(hooked, "use_delay")
    slow_cost = hooked.instructions - start
    assert slow.value == 7
    assert hooked.mem.u32[spins] == 1000

    @hooked.intercept("hal_delay")
    def skip(c: popcorn.Call) -> popcorn.Return:
        assert c.arg(0) == 1000
        return c.returns()

    start = hooked.instructions
    fast = call(hooked, "use_delay")
    assert fast.value == 7
    assert hooked.mem.u32[spins] == 1000
    assert hooked.instructions - start < slow_cost // 100
    assert skip.active


def test_intercepted_removes_the_intercept_at_exit(hooked: popcorn.Machine) -> None:
    spins = hooked.sym("delay_spins")
    with hooked.intercepted("hal_delay", lambda c: c.returns()) as installed:
        call(hooked, "use_delay")
        assert hooked.mem.u32[spins] == 0
    assert not installed.active
    call(hooked, "use_delay")
    assert hooked.mem.u32[spins] == 1000


def test_script_answers_in_order_then_runs_the_original(hooked: popcorn.Machine) -> None:
    hooked.script("next_value", [1, 2])
    call(hooked, "run_script")
    results = hooked.sym("results")
    assert [hooked.mem.i32[results + 4 * i] for i in range(3)] == [1, 2, 100]
    log = hooked.intercept_log("next_value")
    assert [(e.occurrence, e.args, e.value, e.returned) for e in log] == [
        (0, (), 1, True),
        (1, (), 2, True),
        (2, (), 100, True),
    ]


def test_the_log_records_arguments_from_dwarf(hooked: popcorn.Machine) -> None:
    hooked.intercept("triple", lambda c: c.proceed())
    assert call(hooked, "use_triple").value == 123 + 456
    log = hooked.intercept_log("triple")
    assert [(e.args, e.value) for e in log] == [((1, 2, 3), 123), ((4, 5, 6), 456)]


def test_on_return_sees_arguments_value_and_instructions(hooked: popcorn.Machine) -> None:
    seen: list[popcorn.Returned] = []
    hooked.on_return("triple", seen.append)
    call(hooked, "use_triple")
    assert [(r.args, r.value, r.occurrence) for r in seen] == [
        ((1, 2, 3), 123, 0),
        ((4, 5, 6), 456, 1),
    ]
    delays: list[popcorn.Returned] = []
    hooked.on_return("hal_delay", delays.append)
    call(hooked, "use_delay")
    assert [(r.args, r.value) for r in delays] == [((1000,), None)]
    assert delays[0].instructions > 1000


def test_a_stop_reply_stops_the_run_before_the_body(hooked: popcorn.Machine) -> None:
    hooked.intercept("writer", lambda c: c.stop())
    result = call(hooked, "write_three")
    assert result.stop.reason is popcorn.StopReason.USER
    assert result.stop.location.symbol == "writer"
    with pytest.raises(ValueError, match="FAULT"):
        hooked.intercept("note", lambda c: c.stop(popcorn.StopReason.FAULT))
        call(hooked, "note_then_fault")


def failing_handler(c: popcorn.Call) -> popcorn.Return:
    raise LookupError(f"handler failed at {c.function:#x}")


def test_a_handler_error_reaches_the_caller_before_the_fault_of_its_slice(
    hooked: popcorn.Machine,
) -> None:
    hooked.intercept("note", failing_handler)
    with pytest.raises(LookupError, match="handler failed") as caught:
        call(hooked, "note_then_fault")
    frames = [frame.name for frame in traceback.extract_tb(caught.value.__traceback__)]
    assert "failing_handler" in frames
    assert not hooked.busy
    assert call(hooked, "use_triple").value == 579


def test_stub_libc_replaces_the_firmware_routines(hooked: popcorn.Machine) -> None:
    copies = hooked.sym("copies")
    assert call(hooked, "use_libc").value == 5
    assert hooked.mem.u32[copies] == 3
    stubs = hooked.stub_libc()
    assert {s.function for s in stubs} == {
        hooked.sym("memcpy"),
        hooked.sym("memset"),
        hooked.sym("strlen"),
    }
    assert call(hooked, "use_libc").value == 5
    assert hooked.mem.u32[copies] == 3


def test_an_intercept_added_during_a_run_is_refused(hooked: popcorn.Machine) -> None:
    async def scenario() -> None:
        task = asyncio.create_task(hooked.run())
        await asyncio.sleep(0)
        with pytest.raises(popcorn.MachineRunning):
            hooked.intercept("note", lambda c: c.proceed())
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())


def test_install_and_remove_leaves_no_handler_behind(hooked: popcorn.Machine) -> None:
    table = hooked._callback_table  # pyright: ignore[reportPrivateUsage]
    before = len(table)
    for _ in range(1000):
        with hooked.intercepted("note", lambda c: c.proceed()):
            pass
    assert len(table) == before

    def handler(c: popcorn.Call) -> popcorn.Return:
        return c.proceed()

    probe = weakref.ref(handler)
    installed = hooked.intercept("note", handler)
    del handler
    installed.remove()
    gc.collect()
    assert probe() is None


def test_a_call_kept_past_its_handler_is_refused(hooked: popcorn.Machine) -> None:
    stored: list[popcorn.Call] = []

    def keep(c: popcorn.Call) -> popcorn.Return:
        stored.append(c)
        raise KeyError("kept")

    hooked.intercept("note", keep)
    with pytest.raises(KeyError):
        call(hooked, "note_then_fault")
    with pytest.raises(popcorn.ConfigError):
        _ = stored[0].function


def test_a_struct_by_value_parameter_does_not_block_hooks(hooked: popcorn.Machine) -> None:
    errors: list[str] = []

    def pair(c: popcorn.Call) -> popcorn.Return:
        try:
            c.args_typed()
        except TypeError as error:
            errors.append(str(error))
        return c.proceed()

    hooked.intercept("pair_sum", pair)
    returns: list[popcorn.Returned] = []
    hooked.on_return("pair_sum", returns.append)
    assert call(hooked, "use_pair").value == 5
    assert errors and "pair_sum" in errors[0] and "struct" in errors[0].lower()
    assert [r.value for r in returns] == [5]


def test_the_log_pairs_recursive_returns_innermost_first(hooked: popcorn.Machine) -> None:
    hooked.intercept("fact", lambda c: c.proceed())
    assert call(hooked, "use_fact").value == 6
    log = hooked.intercept_log("fact")
    assert [(e.args, e.value) for e in log] == [((3,), 6), ((2,), 2), ((1,), 1)]


def test_the_strlen_stub_counts_raw_bytes(hooked: popcorn.Machine) -> None:
    hooked.stub_libc()
    assert call(hooked, "use_strlen_ff").value == 4


def test_a_handler_error_inside_a_call_restores_the_interrupted_context(
    hooked: popcorn.Machine,
) -> None:
    names = ("r0", "r1", "r2", "r3", "r12", "sp", "lr", "pc", "xpsr")
    before = {name: int(getattr(hooked.reg, name)) for name in names}
    hooked.intercept("note", failing_handler)
    with pytest.raises(LookupError, match="handler failed"):
        call(hooked, "note_then_fault")
    assert {name: int(getattr(hooked.reg, name)) for name in names} == before


def test_record_returns_keeps_the_newest_entries_and_counts_marks(
    hooked: popcorn.Machine,
) -> None:
    recording = hooked.record_returns("writer", lambda r: r.args[0], capacity=4)
    for value in range(10):
        call(hooked, "writer", value)
    assert [e.value for e in recording.since(0)] == [6, 7, 8, 9]
    assert recording.dropped == 6
    assert recording.mark() == 10
    assert [e.value for e in recording.since(8)] == [8, 9]
    assert recording.since(10) == []


def test_a_parked_call_records_one_return_at_its_time(hooked: popcorn.Machine) -> None:
    recording = hooked.record_returns("triple")
    times: list[VirtualTime] = []
    hooked.on_return("triple", lambda _: times.append(hooked.time))
    call(hooked, "triple", 1, 2, 3)
    (entry,) = recording.since()
    assert entry.value.args == (1, 2, 3)
    assert entry.value.value == 123
    assert [entry.at] == times


def test_clear_drops_the_entries_and_marks_count_on(hooked: popcorn.Machine) -> None:
    recording = hooked.record_returns("writer", lambda r: r.args[0])
    call(hooked, "write_three")
    old = recording.mark()
    recording.clear()
    assert recording.since(0) == []
    assert recording.since(old) == []
    call(hooked, "writer", 7)
    assert recording.mark() == old + 1
    assert [e.value for e in recording.since(old)] == [7]
    assert recording.dropped == 0


def test_a_removed_recording_keeps_its_entries(hooked: popcorn.Machine) -> None:
    with hooked.record_returns("writer", lambda r: r.args[0]) as recording:
        call(hooked, "write_three")
    call(hooked, "write_three")
    assert [e.value for e in recording.since()] == [1, 2, 3]
    assert recording.mark() == 3
    assert hooked.remove_hooks() == 0


def test_remove_hooks_stops_a_recording(hooked: popcorn.Machine) -> None:
    recording = hooked.record_returns("writer")
    call(hooked, "write_three")
    assert hooked.remove_hooks() == 1
    call(hooked, "write_three")
    assert recording.mark() == 3


def failing_convert(returned: popcorn.Returned) -> int:
    raise LookupError(f"convert failed at {returned.function:#x}")


def test_a_convert_error_ends_the_run_as_a_handler_error(hooked: popcorn.Machine) -> None:
    recording = hooked.record_returns("writer", failing_convert)
    with pytest.raises(LookupError, match="convert failed"):
        call(hooked, "write_three")
    assert recording.mark() == 0
    assert not hooked.busy


def test_record_returns_refuses_an_empty_capacity(hooked: popcorn.Machine) -> None:
    with pytest.raises(ValueError, match="capacity"):
        hooked.record_returns("writer", capacity=0)
    assert hooked.remove_hooks() == 0


def test_recorded_times_match_the_time_an_on_return_handler_reads(
    m33: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(m33, image=popcorn.Image.elf(CALLEE_ELF))
    m.reset()
    start = m.snapshot()
    recording = m.record_returns("tick", lambda r: r.value)
    asyncio.run(m.run(for_=ms(3)))
    m.remove_hooks()
    m.restore(start)
    seen: list[tuple[VirtualTime, int | float | None]] = []
    m.on_return("tick", lambda r: seen.append((m.time, r.value)))
    asyncio.run(m.run(for_=ms(3)))
    assert len(seen) >= 10
    assert [(e.at, e.value) for e in recording.since()] == seen


def test_a_recording_removed_by_a_handler_stops_at_once(hooked: popcorn.Machine) -> None:
    recordings: list[popcorn.Recording[popcorn.Returned]] = []
    hooked.on_return("writer", lambda _: recordings[0].remove())
    recording = hooked.record_returns("writer")
    recordings.append(recording)
    call(hooked, "write_three")
    assert recording.mark() == 1
    assert hooked.remove_hooks() == 1
    call(hooked, "write_three")
    assert recording.mark() == 1


ISR_PHASE = 25
ISR_ROUNDS = 4 * ISR_PHASE
ISR_HANDLERS = (
    "tail_handler",
    "irq_exit",
    "first_handler",
    "second_handler",
    "low_handler",
    "mid_handler",
    "fpu_handler",
    "fpu_exit",
    "svc_handler",
)


def isr_machine(m33: popcorn.PartDescription) -> popcorn.Machine:
    """A reset machine over the isr firmware: each round pends a handler that tail-calls
    `irq_exit`, two handlers that tail-chain, a low handler that a mid handler preempts, itself
    preempted by a high one, and a handler that tail-calls the float `fpu_exit`. Four phases of
    `ISR_PHASE` rounds: thread on MSP, then on PSP, each with basic then extended frames. On PSP,
    an SVC before and after each round moves the thread between stacks: `svc_handler` returns
    another EXC_RETURN than it got."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(ISR_ELF))
    m.reset()
    return m


def isr_done(m: popcorn.Machine) -> bool:
    """Whether the firmware finished its rounds and idles in thread mode."""
    return int(m.var("done").value) == 1


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_record_returns_counts_each_exception_return_of_a_handler_or_its_tail_call(
    m33: popcorn.PartDescription, running: bool
) -> None:
    m = isr_machine(m33)
    recordings = {name: m.record_returns(name) for name in ISR_HANDLERS}

    async def scenario() -> None:
        if running:
            async with m.running():
                await m.wait_until(isr_done)
        else:
            assert (await m.run(budget=1_000_000)).reason is popcorn.StopReason.IDLE

    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    run_bounded(scenario(), 10)
    if not running:
        assert m._handle.gil_entries() == before  # pyright: ignore[reportPrivateUsage]
    assert isr_done(m)
    for counter in ("tail", "first", "second", "low", "mid", "high", "fpu"):
        assert int(m.var(f"{counter}_entries").value) == ISR_ROUNDS
    assert int(m.var("switches").value) == ISR_ROUNDS
    for name, recording in recordings.items():
        entries = recording.since()
        assert len(entries) == ISR_ROUNDS, name
        times = [e.at for e in entries]
        assert times == sorted(set(times)), name
    exits = [e.value for e in recordings["irq_exit"].since()]
    assert [r.args for r in exits] == [(0,)] * ISR_ROUNDS
    assert [r.value for r in exits] == [10 * (n + 1) for n in range(ISR_ROUNDS)]
    assert [e.value.value for e in recordings["tail_handler"].since()] == [None] * ISR_ROUNDS
    floats = [e.value.value for e in recordings["fpu_exit"].since()]
    assert floats == [0.5 * (n + 1) for n in range(ISR_ROUNDS)]


def test_an_exception_return_counts_after_the_handler_body_and_before_a_tail_chain(
    m33: popcorn.PartDescription,
) -> None:
    m = isr_machine(m33)
    seen: dict[str, list[tuple[int, int, int]]] = {}

    def watching(name: str, leaves: str, other: str) -> None:
        def returned(r: popcorn.Returned) -> None:
            row = (r.occurrence, int(m.var(leaves).value), int(m.var(other).value))
            seen.setdefault(name, []).append(row)

        m.on_return(name, returned)

    watching("tail_handler", "exits", "tail_entries")
    watching("first_handler", "first_leaves", "second_entries")
    watching("mid_handler", "mid_leaves", "high_leaves")
    watching("low_handler", "low_leaves", "mid_leaves")
    watching("svc_handler", "switches", "tail_entries")
    entered_with: list[int] = []

    @m.intercept("tail_handler")
    def _(c: popcorn.Call) -> popcorn.Return:  # pyright: ignore[reportUnusedFunction]
        entered_with.append(c.lr)
        return c.proceed()

    assert asyncio.run(m.run(budget=1_000_000)).reason is popcorn.StopReason.IDLE
    assert isr_done(m)
    rounds = range(ISR_ROUNDS)
    assert seen["tail_handler"] == [(n, n + 1, n + 1) for n in rounds]
    assert seen["first_handler"] == [(n, n + 1, n) for n in rounds]
    assert seen["mid_handler"] == [(n, n + 1, n + 1) for n in rounds]
    assert seen["low_handler"] == [(n, n + 1, n + 1) for n in rounds]
    assert [row[:2] for row in seen["svc_handler"]] == [(n, n + 1) for n in rounds]
    phases = (0xFFFFFFF8, 0xFFFFFFFC, 0xFFFFFFE8, 0xFFFFFFEC)
    assert entered_with == [lr for lr in phases for _ in range(ISR_PHASE)]


def test_a_raising_handler_on_an_exception_return_leaves_the_exit_completed(
    m33: popcorn.PartDescription,
) -> None:
    m = isr_machine(m33)
    handlers = m.record_returns("tail_handler")
    exits = m.record_returns("irq_exit")
    raised: list[int] = []

    def first_raises(r: popcorn.Returned) -> None:
        if not raised:
            raised.append(r.occurrence)
            raise ValueError("first exit")

    m.on_return("irq_exit", first_raises)
    with pytest.raises(ValueError, match="first exit"):
        asyncio.run(m.run(budget=1_000_000))
    stop = asyncio.run(m.run(budget=1_000_000))
    assert stop.reason is popcorn.StopReason.IDLE
    assert isr_done(m)
    assert raised == [0]
    assert [e.value.value for e in exits.since()] == [10 * (n + 1) for n in range(1, ISR_ROUNDS)]
    assert [e.value.occurrence for e in handlers.since()] == list(range(ISR_ROUNDS - 1))

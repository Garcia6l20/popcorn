"""Intercepts, scripted replies, the intercept log, return hooks and libc stubs."""

from __future__ import annotations

import asyncio
import gc
import traceback
import weakref

import popcorn
import pytest


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

"""Accesses of a hosted machine: sync parked, refused running, `m.read` and `m.write` over
`m.ref`, and the run control around them."""

from __future__ import annotations

import asyncio
import contextlib
import contextvars
import gc
import sys
import threading
import weakref
from collections.abc import Callable
from typing import Any

import popcorn
import pytest
from popcorn._refs import Pending, Ref, VarRef
from popcorn.arm import Reg
from popcorn.board import AudioOptions, UsbHostOptions
from popcorn.image.dwarf import BaseType, DebugInfo, Encoding, Member, StructType
from popcorn.units import VirtualTime, ms, s, us

from .conftest import (
    BOARD_ELF,
    DRIVER_ELF,
    M33_JSON,
    REFUSED_INSIDE,
    REQUESTS_INSIDE,
    VIEWS_ELF,
    Machines,
    requests_inside,
    run_bounded,
)

HANG = 10.0

SRAM = 0x2000_8000
UNMAPPED = 0x6000_0000


class Never:
    def ready(self, machine: popcorn.Machine) -> bool:
        return False


def test_sync_accesses_inside_running_are_machine_running(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)

    async def scenario() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                _ = m.mem.u32[SRAM]
            with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
                m.reg.r0 = 1

    asyncio.run(scenario())


def test_sync_accesses_stay_refused_after_the_free_run_ended_by_itself(
    machines: Machines,
) -> None:
    m = machines("fault")

    async def scenario() -> None:
        async with m.running():
            ended = await m.wait_for(Never())
            assert ended.reason is popcorn.StopReason.FAULT
            assert m.busy
            with pytest.raises(popcorn.MachineRunning):
                _ = m.pc
            with pytest.raises(popcorn.MachineRunning):
                m.mem.u32[SRAM] = 1
            (pc,) = await m.read(m.ref.pc)
            assert pc == ended.pc

    with pytest.raises(popcorn.FaultError):
        run_bounded(scenario())
    assert not m.busy


def test_a_parked_read_returns_at_once_at_the_machine_time(machines: Machines) -> None:
    m = machines()
    pending = m.read(m.ref.pc, m.ref.instructions)
    assert pending.done()
    values = asyncio.run(pending)
    assert values == (m.pc, m.instructions)
    assert values.time == m.time
    assert isinstance(values, popcorn.Readout)
    assert asyncio.run(m.read()).time == m.time


def test_two_writes_of_one_register_apply_in_order(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.write((m.ref.reg.r0, 1), (m.ref.reg.r0, 2)))
    assert m.reg.r0 == 2


def test_a_failing_op_stops_the_batch_after_the_ops_before_it(machines: Machines) -> None:
    m = machines()
    m.mem.u32[SRAM] = 0
    with pytest.raises(popcorn.EmulationError) as caught:
        asyncio.run(m.write((m.ref.mem.u32[SRAM], 7), (m.ref.mem.u32[UNMAPPED], 1)))
    assert any("argument 1" in note for note in caught.value.__notes__)
    assert m.mem.u32[SRAM] == 7


def test_a_failing_parked_batch_leaves_its_machine_collectable(machines: Machines) -> None:
    m = machines()
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(m.write((m.ref.mem.u32[SRAM], 7), (m.ref.mem.u32[UNMAPPED], 1)))
    with pytest.raises(popcorn.EmulationError):
        asyncio.run(m.read(m.ref.mem.u32[UNMAPPED]))
    probe = weakref.ref(m)
    del m
    gc.collect()
    assert probe() is None


def test_done_follows_the_command_not_the_await(machines: Machines) -> None:
    m = machines("idle")
    asyncio.run(m.run())
    pending = m.read(m.ref.pc, at=m.time + us(100))

    async def scenario() -> bool:
        task = asyncio.ensure_future(pending)
        await asyncio.sleep(0)
        started = pending.done()
        await m.run(for_=ms(1))
        await task
        return started

    assert not asyncio.run(scenario())
    assert pending.done()


def test_a_past_stamp_on_a_parked_machine_raises_at_the_call(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=us(10)))
    with pytest.raises(ValueError, match="before machine time"):
        m.write((m.ref.reg.r0, 1), at=VirtualTime(0))


def test_a_stamped_write_waits_for_the_run_that_reaches_it(machines: Machines) -> None:
    m = machines()
    at = m.time + us(5)
    pending = m.write((m.ref.mem.u32[SRAM], 0x5A), at=at)
    assert not pending.done()

    async def scenario() -> VirtualTime:
        await m.run(for_=us(20))
        return (await pending).time

    assert asyncio.run(scenario()) >= at
    assert m.mem.u32[SRAM] == 0x5A


def test_a_stamped_read_on_an_idle_core_runs_exactly_at_its_time(machines: Machines) -> None:
    m = machines("idle")
    asyncio.run(m.run())
    at = m.time + us(250)
    pending = m.read(m.ref.instructions, at=at)

    async def scenario() -> VirtualTime:
        await m.run(for_=ms(1))
        return (await pending).time

    assert asyncio.run(scenario()) == at


@pytest.mark.parametrize("move", ["reset", "restore"])
def test_a_stamped_write_superseded_while_parked_raises(machines: Machines, move: str) -> None:
    m = machines()
    saved = m.snapshot()
    pending = m.write((m.ref.reg.r0, 1), at=m.time + us(5))
    if move == "reset":
        m.reset()
    else:
        m.restore(saved)

    async def scenario() -> None:
        await m.run(for_=us(20))
        await pending

    with pytest.raises(popcorn.EmulationError, match=f"superseded by a {move}"):
        asyncio.run(scenario())


@pytest.mark.parametrize("move", ["reset", "restore"])
def test_a_stamped_read_superseded_inside_a_parked_section_fails_at_once(
    machines: Machines, move: str
) -> None:
    m = machines()
    saved = m.snapshot()
    pending = m.read(m.ref.reg.r0, at=m.time + us(10))

    def moved() -> None:
        if move == "reset":
            m.reset()
        else:
            m.restore(saved)

    m._handle.parked(moved)  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        await pending

    with pytest.raises(popcorn.EmulationError, match=f"superseded by a {move}"):
        run_bounded(scenario(), 1)


@pytest.mark.parametrize("move", ["reset", "restore"])
def test_a_stamped_read_superseded_while_parked_fails_at_once(
    machines: Machines, move: str
) -> None:
    m = machines()
    saved = m.snapshot()
    pending = m.read(m.ref.reg.r0, at=m.time + us(10))
    if move == "reset":
        m.reset()
    else:
        m.restore(saved)

    async def scenario() -> None:
        await pending

    with pytest.raises(popcorn.EmulationError, match=f"superseded by a {move}"):
        run_bounded(scenario(), 1)


def test_a_dropped_stamped_write_reports_its_later_failure(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    m = machines()
    reported: list[tuple[str | None, str]] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append((raised.err_msg, str(raised.exc_value)))

    monkeypatch.setattr(sys, "unraisablehook", report)
    pending = m.write((m.ref.reg.r0, 5), at=m.time + us(10))
    del pending
    assert reported == []
    m.reset()
    assert reported == [
        ("a command whose Pending was dropped failed later", "superseded by a reset")
    ]


def test_a_stamped_write_cancelled_before_its_first_step_never_applies(
    machines: Machines,
) -> None:
    m = machines()
    m.mem.u32[SRAM] = 0

    async def scenario() -> None:
        task = asyncio.ensure_future(m.write((m.ref.mem.u32[SRAM], 9), at=m.time + us(5)))
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        await m.run(for_=us(20))

    asyncio.run(scenario())
    assert m.mem.u32[SRAM] == 0


def test_a_pending_thrown_the_legacy_way_keeps_the_message(machines: Machines) -> None:
    m = machines()
    pending = m.write((m.ref.mem.u32[SRAM], 9), at=m.time + us(5))
    with pytest.raises(ValueError, match="the message"):
        pending.throw(ValueError, "the message")
    asyncio.run(m.run(for_=us(20)))
    assert m.mem.u32[SRAM] == 9


def test_a_cancelled_stamped_write_never_applies(machines: Machines) -> None:
    m = machines()
    m.mem.u32[SRAM] = 0

    async def scenario() -> None:
        task = asyncio.ensure_future(m.write((m.ref.mem.u32[SRAM], 9), at=m.time + us(5)))
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        await m.run(for_=us(20))

    asyncio.run(scenario())
    assert m.mem.u32[SRAM] == 0


def test_read_and_write_from_a_handler_raise(machines: Machines) -> None:
    m = machines(slice_instructions=1_000)
    other = machines()
    errors: list[BaseException] = []

    def predicate(machine: popcorn.Machine) -> bool:
        for target in (machine, other):
            try:
                target.read(target.ref.pc)
            except RuntimeError as error:
                errors.append(error)
        return True

    asyncio.run(m.run(stop_when=predicate))
    assert len(errors) == 2
    assert all("handler accesses the machine directly" in str(e) for e in errors)


def test_a_ref_of_another_machine_is_refused(machines: Machines) -> None:
    m, other = machines(), machines()
    with pytest.raises(ValueError, match="another machine"):
        m.read(other.ref.pc)
    with pytest.raises(TypeError, match="read-only"):
        m.write((m.ref.pc, 0))
    with pytest.raises(TypeError, match="pair"):
        m.write()


def test_halt_start_and_running_edges(machines: Machines) -> None:
    m = machines()
    m.halt()
    m.start()
    try:
        with pytest.raises(popcorn.MachineBusy):
            m.start()
    finally:
        m.halt()
    assert not m.busy

    faulty = machines("fault")

    async def faulting() -> None:
        async with faulty.running():
            await asyncio.sleep(0.01)

    with pytest.raises(popcorn.FaultError):
        run_bounded(faulting())

    ended = machines("fault")

    async def waiting() -> tuple[popcorn.Stop, popcorn.Stop]:
        ended.start()
        try:
            first = await ended.wait_for(Never())
            return first, await ended.wait_for(Never(), for_=ms(1))
        finally:
            with pytest.raises(popcorn.FaultError):
                ended.halt()

    first, second = run_bounded(waiting())
    assert first == second
    assert first.reason is popcorn.StopReason.FAULT


def test_a_cancelled_call_leaves_no_call_in_progress(callee: popcorn.Machine) -> None:
    async def scenario() -> None:
        task = asyncio.create_task(callee.call("forever"))
        await asyncio.sleep(0.005)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert not callee._handle.in_call()  # pyright: ignore[reportPrivateUsage]


def test_a_condition_evaluates_parked_as_its_sync_view(machines: Machines) -> None:
    m = machines()
    condition = m.ref.mem.u32[SRAM].eq(5)
    masked = m.ref.mem.u32[SRAM].masked(0xF0).ge(0x50)
    m.mem.u32[SRAM] = 5
    assert condition.ready(m) is True
    assert masked.ready(m) is False
    m.mem.u32[SRAM] = 0x56
    assert condition.ready(m) is False
    assert masked.ready(m) is True
    assert condition.label == "mem.u32[0x20008000].eq(5)"
    with pytest.raises(AttributeError):
        condition.label = "renamed"  # pyright: ignore[reportAttributeAccessIssue]


def test_a_condition_is_refused_where_it_cannot_run_natively(
    machines: Machines, views: popcorn.Machine
) -> None:
    m = machines()
    with pytest.raises(ValueError, match="0x40010000 is not plain memory"):
        m.ref.mem.u32[0x4001_0000].eq(0)
    with pytest.raises(ValueError, match=f"{UNMAPPED:#010x} is not plain memory"):
        m.ref.mem.u32[UNMAPPED].eq(0)
    with pytest.raises(ValueError, match="-1 is outside"):
        m.ref.mem.u8[SRAM].gt(-1)
    with pytest.raises(ValueError, match="256 is outside"):
        m.ref.mem.u8[SRAM].eq(256)
    with pytest.raises(TypeError, match="is an integer"):
        m.ref.mem.u8[SRAM].eq(1.5)  # pyright: ignore[reportArgumentType]
    with pytest.raises(TypeError, match="takes no mask"):
        m.ref.mem.f32[SRAM].masked(1)
    with pytest.raises(ValueError, match="does not fit"):
        m.ref.mem.u8[SRAM].masked(0x100)
    with pytest.raises(AttributeError):
        m.ref.reg.r0.eq(0)  # pyright: ignore[reportAttributeAccessIssue, reportUnknownMemberType]
    with pytest.raises(TypeError, match="spans 9 bytes"):
        views.ref.var("odd").big.eq(7)
    with pytest.raises(TypeError, match="eq and ne only"):
        views.ref.var("armed").lt(True)
    with pytest.raises(TypeError, match="not a scalar"):
        views.ref.var("table").eq(0)


def views_at_loop(m33: popcorn.MachineDescription) -> popcorn.Machine:
    """A machine over the views firmware, past its bkpt in the endless loop after it."""
    m = popcorn.Machine(m33, image=popcorn.Image.elf(VIEWS_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


def board_with_links(u575: popcorn.MachineDescription) -> popcorn.Machine:
    """A U575 machine over the board firmware, at `ready`, with a USB host and an audio
    block."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(BOARD_ELF),
        usb={"OTG_FS": UsbHostOptions(attached=False)},
        audio={"SAI1": {"A": AudioOptions()}},
    )
    m.reset()
    assert asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000)).reason is (
        popcorn.StopReason.ADDRESS
    )
    return m


type RefOf = Callable[[popcorn.Machine], Ref[Any]]

VIEWS_KINDS: dict[str, tuple[RefOf, object]] = {
    "u8": (lambda m: m.ref.mem.u8[m.sym("table")], 0xA5),
    "u16": (lambda m: m.ref.mem.u16[m.sym("table")], 0xBEEF),
    "u32": (lambda m: m.ref.mem.u32[m.sym("table")], 0xDEAD_BEEF),
    "u64": (lambda m: m.ref.mem.u64[m.sym("table")], 0x0123_4567_89AB_CDEF),
    "i8": (lambda m: m.ref.mem.i8[m.sym("table")], -5),
    "i16": (lambda m: m.ref.mem.i16[m.sym("table")], -300),
    "i32": (lambda m: m.ref.mem.i32[m.sym("table")], -70_000),
    "i64": (lambda m: m.ref.mem.i64[m.sym("table")], -(2**40)),
    "f32": (lambda m: m.ref.mem.f32[m.sym("table")], 1.5),
    "f64": (lambda m: m.ref.mem.f64[m.sym("table")], -2.25),
    "block": (lambda m: m.ref.mem.block(m.sym("table"), 8), b"\x01\x02\x03\x04\x05\x06\x07\x08"),
    "cstring": (lambda m: m.ref.mem.cstring(m.sym("banner")), None),
    "reg-name": (lambda m: m.ref.reg.r4, 0x1234),
    "reg-enum": (lambda m: m.ref.reg[Reg.R5], 0x5678),
    "pc": (lambda m: m.ref.pc, None),
    "instructions": (lambda m: m.ref.instructions, None),
    "pending-events": (lambda m: m.ref.pending_events, None),
    "var-member": (lambda m: m.ref.var("table").count, 9),
    "var-element-float": (lambda m: m.ref.var("table").entries[1].gain, 6.5),
    "var-element-int": (lambda m: m.ref.var("matrix")[1][2], -42),
    "var-bit-field": (lambda m: m.ref.var("table").mode, 3),
    "var-signed-bit-field": (lambda m: m.ref.var("wide").high, -100),
    "var-bool": (lambda m: m.ref.var("armed"), False),
    "var-enum-name": (lambda m: m.ref.var("shadowed").kind.value.enum_name, None),
    "masked": (lambda m: m.ref.mem.i8[m.sym("table")].masked(0xF0), None),
}

BOARD_KINDS: dict[str, tuple[RefOf, object]] = {
    "register": (lambda m: m.ref.periph.GPIOB.GPIO_MODER, 0x5555_5555),
    "register-bus": (lambda m: m.ref.periph.GPIOB.GPIO_ODR.bus, 0x0003),
    "register-field": (lambda m: m.ref.periph.GPIOB.GPIO_MODER.MODE1, 2),
    "pin-level": (lambda m: m.ref.periph.GPIOA.pin(0).level, True),
    "pin-release": (lambda m: m.ref.periph.GPIOA.pin(0).level, None),
    "pin-mode": (lambda m: m.ref.periph.GPIOA.pin(5).mode, None),
    "pin-alternate": (lambda m: m.ref.periph.GPIOA.pin(5).alternate, None),
    "adc-volts": (lambda m: m.ref.periph.ADC1.input(5).volts, 1.5),
    "audio-clock": (lambda m: m.ref.periph.SAI1.audio("A").external_frame_hz, 48_000),
    "port-queued": (lambda m: m.ref.periph.USART1.port.queued, None),
    "host-state": (lambda m: m.ref.periph.OTG_FS.host.state, None),
    "host-address": (lambda m: m.ref.periph.OTG_FS.host.address, None),
    "host-configuration": (lambda m: m.ref.periph.OTG_FS.host.configuration, None),
    "host-requests": (lambda m: m.ref.periph.OTG_FS.host.requests, None),
}


def equivalent(build: Callable[[], popcorn.Machine], ref_of: RefOf, value: object) -> None:
    """A parked `m.read` returns the sync getter's value, an `m.write` leaves what the sync
    setter leaves on a twin, and both run on the machine's thread without the GIL."""
    m = build()
    ref = ref_of(m)
    (read,) = asyncio.run(m.read(ref))
    assert read == ref.get()
    if ref.writable:
        twin = build()
        twin_ref = ref_of(twin)
        twin_ref.put(value)
        asyncio.run(m.write((ref, value)))
        assert ref.get() == twin_ref.get()

    async def running() -> None:
        async with m.running():
            await m.read(ref)
            if ref.writable:
                await m.write((ref, value))

    run_bounded(running(), HANG)
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


@pytest.mark.parametrize("kind", list(VIEWS_KINDS))
def test_a_ref_reads_and_writes_as_its_sync_view_without_the_gil(
    m33: popcorn.MachineDescription, kind: str
) -> None:
    ref_of, value = VIEWS_KINDS[kind]
    equivalent(lambda: views_at_loop(m33), ref_of, value)


@pytest.mark.parametrize("kind", list(BOARD_KINDS))
def test_a_board_ref_reads_and_writes_as_its_sync_view_without_the_gil(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    ref_of, value = BOARD_KINDS[kind]
    equivalent(lambda: board_with_links(u575), ref_of, value)


def test_a_failing_op_names_its_argument_and_stops_the_batch(machines: Machines) -> None:
    m = machines()
    with pytest.raises(popcorn.EmulationError) as raised:
        asyncio.run(m.read(m.ref.pc, m.ref.mem.u32[UNMAPPED]))
    assert f"reading argument 1, mem.u32[{UNMAPPED:#010x}]" in raised.value.__notes__
    m.mem.u32[SRAM] = 0
    with pytest.raises(popcorn.EmulationError) as raised:
        asyncio.run(m.write((m.ref.mem.u32[SRAM], 7), (m.ref.mem.u32[UNMAPPED], 1)))
    assert f"writing argument 1, mem.u32[{UNMAPPED:#010x}]" in raised.value.__notes__
    assert m.mem.u32[SRAM] == 7


def test_a_write_validates_at_the_call(u575: popcorn.MachineDescription) -> None:
    m = board_with_links(u575)
    with pytest.raises(ValueError, match="not a voltage"):
        m.write((m.ref.periph.ADC1.input(5).volts, -1.0))
    with pytest.raises(ValueError, match="does not fit"):
        m.write((m.ref.mem.u8[SRAM], 256))


def test_a_variable_write_validates_at_the_call(m33: popcorn.MachineDescription) -> None:
    m = views_at_loop(m33)
    with pytest.raises(ValueError, match="does not fit"):
        m.write((m.ref.var("table").mode, 99))
    with pytest.raises(TypeError, match="bitfield"):
        m.write((m.ref.var("table").mode, 1.5))


def test_a_field_write_keeps_the_siblings_written_after_its_post(
    u575: popcorn.MachineDescription, m33: popcorn.MachineDescription
) -> None:
    board = board_with_links(u575)
    moder = board.periph.GPIOB.GPIO_MODER
    moder.value = 0
    pending = board.write((board.ref.periph.GPIOB.GPIO_MODER.MODE1, 2), at=board.time + us(5))
    moder.MODE0 = 1
    asyncio.run(board.run(for_=us(10)))
    run_bounded(pending, HANG)
    assert (moder.MODE0, moder.MODE1) == (1, 2)

    views = views_at_loop(m33)
    wide = views.var("wide")
    pending = views.write((views.ref.var("wide").mid, 0x123), at=views.time + us(5))
    wide.low.value = 33
    wide.high.value = -7
    asyncio.run(views.run(for_=us(10)))
    run_bounded(pending, HANG)
    assert (wide.low.value, wide.mid.value, wide.high.value) == (33, 0x123, -7)


def test_a_float_too_large_for_its_ref_is_a_value_error(machines: Machines) -> None:
    m = machines()
    with pytest.raises(ValueError, match="does not fit"):
        m.write((m.ref.mem.f32[SRAM], 1e40))
    with pytest.raises(ValueError, match="does not fit"):
        m.mem.f32[SRAM] = 1e40


def test_a_dropped_stamped_write_left_queued_at_close_is_not_reported(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported: list[str | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.err_msg)

    monkeypatch.setattr(sys, "unraisablehook", report)
    m = popcorn.Machine(
        popcorn.load_description(str(M33_JSON)), image=popcorn.Image.elf(str(DRIVER_ELF))
    )
    m.reset()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    del pending
    del m
    gc.collect()
    assert reported == []


@pytest.mark.parametrize("started", [False, True], ids=["unstarted", "started"])
def test_a_stamped_write_whose_await_was_thrown_into_reports_a_later_failure(
    monkeypatch: pytest.MonkeyPatch, started: bool
) -> None:
    reported: list[str | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.err_msg)

    monkeypatch.setattr(sys, "unraisablehook", report)
    m = popcorn.Machine(
        popcorn.load_description(str(M33_JSON)), image=popcorn.Image.elf(str(DRIVER_ELF))
    )
    m.reset()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))

    async def scenario(thrown: Pending[object]) -> None:
        if started:
            thrown.send(None)
        with pytest.raises(ValueError, match="thrown"):
            thrown.throw(ValueError("thrown"))

    asyncio.run(scenario(pending))
    del pending
    del m
    gc.collect()
    assert reported == ["a command whose await was thrown into failed later"]


def _reports(monkeypatch: pytest.MonkeyPatch) -> list[tuple[str | None, str]]:
    reported: list[tuple[str | None, str]] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append((raised.err_msg, str(raised.exc_value)))

    monkeypatch.setattr(sys, "unraisablehook", report)
    return reported


def _driver_machine() -> popcorn.Machine:
    m = popcorn.Machine(
        popcorn.load_description(str(M33_JSON)), image=popcorn.Image.elf(str(DRIVER_ELF))
    )
    m.reset()
    return m


def test_a_stamped_write_dropped_after_its_machine_closed_is_not_reported(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    del m
    gc.collect()
    del pending
    gc.collect()
    assert reported == []


def test_a_superseded_stamped_write_dropped_later_is_reported_at_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    m.reset()
    del pending
    assert [message for message, _ in reported] == [
        "a command whose Pending was dropped failed later"
    ]
    assert "superseded by a reset" in reported[0][1]


def test_a_cancel_after_the_command_failed_reports_the_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()

    async def scenario() -> None:
        task = asyncio.ensure_future(m.write((m.ref.reg.r0, 5), at=m.time + ms(10)))
        await asyncio.sleep(0)
        m.reset()
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert [message for message, _ in reported] == ["a command its awaiter cancelled failed later"]
    assert "superseded by a reset" in reported[0][1]


class _SupersededBeforeCancel:
    """A command's future whose cancel lands after a supersede, as a reset on another thread
    between the cancel's done check and its withdraw."""

    def __init__(self, future: Any, supersede: Callable[[], None]) -> None:
        self._future = future
        self._supersede = supersede

    def __getattr__(self, name: str) -> Any:
        return getattr(self._future, name)

    def cancel(self) -> bool:
        self._supersede()
        return bool(self._future.cancel())


def test_a_supersede_landing_before_a_cancel_withdraws_is_reported(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    slot = "_future"
    setattr(pending, slot, _SupersededBeforeCancel(getattr(pending, slot), m.reset))

    async def scenario() -> None:
        task = asyncio.ensure_future(pending)
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert [message for message, _ in reported] == ["a command its awaiter cancelled failed later"]
    assert "superseded by a reset" in reported[0][1]


def test_a_cancel_before_the_first_step_that_cannot_withdraw_reports_a_later_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    slot = "_future"
    setattr(pending, slot, _SupersededBeforeCancel(getattr(pending, slot), m.reset))

    async def scenario() -> None:
        task = asyncio.ensure_future(pending)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario())
    assert [message for message, _ in reported] == ["a command its awaiter cancelled failed later"]
    assert "superseded by a reset" in reported[0][1]


def test_a_failure_reported_by_a_collection_on_the_machine_thread_reaches_the_hook_off_it(
    machines: Machines, monkeypatch: pytest.MonkeyPatch
) -> None:
    reported: list[tuple[str | None, int]] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append((raised.err_msg, threading.get_ident()))

    monkeypatch.setattr(sys, "unraisablehook", report)
    m = machines(slice_instructions=1_000)
    held = [m.write((m.ref.reg.r0, 5), at=m.time + ms(10))]
    m.reset()

    def drop(_: object) -> bool:
        held.clear()
        return True

    asyncio.run(m.run(stop_when=drop))
    assert held == []
    assert reported == [("a command whose Pending was dropped failed later", threading.get_ident())]


def test_a_cancelled_stamped_write_that_was_withdrawn_is_not_reported(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()

    async def scenario(machine: popcorn.Machine) -> None:
        task = asyncio.ensure_future(
            machine.write((machine.ref.reg.r0, 5), at=machine.time + ms(10))
        )
        await asyncio.sleep(0)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(scenario(m))
    del m
    gc.collect()
    assert reported == []


def test_a_pending_driven_outside_a_loop_reports_with_the_await_failure_text(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    reported = _reports(monkeypatch)
    m = _driver_machine()
    pending = m.write((m.ref.reg.r0, 5), at=m.time + ms(10))
    with pytest.raises(RuntimeError, match="no running event loop"):
        pending.send(None)
    del pending
    del m
    gc.collect()
    assert [message for message, _ in reported] == [
        "a command whose await failed before its result failed later"
    ]


def test_a_stamped_write_whose_task_is_destroyed_pending_is_withdrawn() -> None:
    m = _driver_machine()
    m.mem.u32[SRAM] = 0
    loop = asyncio.new_event_loop()

    async def holder(written: Pending[object]) -> None:
        await written

    task = loop.create_task(holder(m.write((m.ref.mem.u32[SRAM], 9), at=m.time + ms(1))))
    loop.run_until_complete(asyncio.sleep(0.01))
    loop.close()
    del task
    gc.collect()
    asyncio.run(m.run(for_=ms(2)))
    assert m.mem.u32[SRAM] == 0


def test_a_run_whose_task_is_destroyed_pending_leaves_the_machine_parked() -> None:
    m = _driver_machine()
    loop = asyncio.new_event_loop()

    async def holder() -> None:
        await m.run(for_=ms(100_000))

    task = loop.create_task(holder())
    loop.run_until_complete(asyncio.sleep(0.01))
    loop.close()
    del task
    gc.collect()
    assert m.mem.u32[SRAM] is not None


def test_a_ref_reaches_a_member_named_value(views: popcorn.Machine) -> None:
    shadowed = views.ref.var("shadowed")
    kind, size, address = asyncio.run(
        views.read(shadowed.kind.value, shadowed.size, shadowed.address)
    )
    assert (kind, size, address) == (7, 7, 0x1234)
    (same,) = asyncio.run(views.read(shadowed.member("kind").member("value")))
    assert same == 7


def test_a_ref_member_lookup_names_the_members(views: popcorn.Machine) -> None:
    with pytest.raises(AttributeError, match="members: size, address, kind"):
        views.ref.var("shadowed").member("value")
    with pytest.raises(AttributeError, match="not a struct or union"):
        _ = views.ref.var("counter").label_text
    with pytest.raises(AttributeError):
        _ = views.ref.var("shadowed")._private


def test_a_ref_reads_an_enum_name_parked_and_running(views: popcorn.Machine) -> None:
    kind = views.ref.var("shadowed").kind.value
    (parked,) = asyncio.run(views.read(kind.enum_name))
    assert parked == "ROUTE_CAN"
    with pytest.raises(TypeError, match=r"shadowed\.size is not an enum"):
        _ = views.ref.var("shadowed").size.enum_name

    async def scenario() -> str | None:
        async with views.running():
            (running,) = await views.read(views.ref.var("shadowed").kind.value.enum_name)
            return running

    assert run_bounded(scenario()) == "ROUTE_CAN"


def test_building_a_var_ref_while_running_reads_no_memory(views: popcorn.Machine) -> None:
    async def scenario() -> tuple[int, str | None]:
        async with views.running():
            with pytest.raises(AttributeError, match="not a struct or union"):
                _ = views.ref.var("table").count.value
            shadowed = views.ref.var("shadowed")
            refs = (shadowed.kind.value, shadowed.kind.value.enum_name, shadowed.size)
            with pytest.raises(popcorn.MachineRunning):
                _ = views.var("shadowed").kind.member("value").value
            kind, name, _ = await views.read(*refs)
            return kind, name

    assert run_bounded(scenario()) == (7, "ROUTE_CAN")


def test_a_member_named_member_is_reached_through_member(views: popcorn.Machine) -> None:
    u32 = BaseType("uint32_t", 4, Encoding.UNSIGNED)
    members = (Member("member", 0, 1, None, None), Member("x", 4, 1, None, None))
    info = DebugInfo(
        by_id={1: u32, 2: StructType("s", 8, members, False, None)},
        types={"struct s": 2},
        variables={},
        functions={},
    )
    views.mem.write(SRAM, (5).to_bytes(4, "little") + (6).to_bytes(4, "little"))
    var = popcorn.Var(views.mem, info, 2, SRAM, "s")
    ref: VarRef[int] = VarRef(views.ref.pc.owner, var)
    assert callable(var.member) and callable(ref.member)
    assert var.member("member").value == 5
    assert var.member("member").name == "s.member"
    assert asyncio.run(views.read(ref.member("member"), ref.x)) == (5, 6)


def test_sample_parked_returns_at_once_and_keeps_the_machine_parked(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    before = (m.time, m.instructions)

    async def scenario() -> tuple[int, VirtualTime]:
        sampled = m.sample(lambda: (int(m.var("alu_value").value), m.time))
        assert sampled.done()
        return await sampled

    value, at = asyncio.run(scenario())
    assert value == int(m.var("alu_value").value)
    assert at == before[0]
    assert (m.time, m.instructions) == before
    assert not m.busy


def test_sample_at_a_later_time_runs_when_a_run_reaches_it(machines: Machines) -> None:
    m = machines()
    start = m.time

    async def scenario() -> VirtualTime:
        sampled = m.sample(lambda: m.time, at=start + ms(1))
        await m.run(for_=ms(2))
        return await sampled

    assert asyncio.run(scenario()) >= start + ms(1)


def test_sample_at_a_past_time_of_a_parked_machine_is_a_value_error(machines: Machines) -> None:
    m = machines()
    asyncio.run(m.run(for_=ms(1)))
    with pytest.raises(ValueError):
        m.sample(lambda: None, at=VirtualTime(0))


@pytest.mark.parametrize("name", REQUESTS_INSIDE)
def test_a_request_inside_a_parked_sample_raises(machines: Machines, name: str) -> None:
    m = machines()
    request, error = requests_inside(m)[name]

    async def scenario() -> object:
        return await m.sample(request)

    with pytest.raises(error, match=REFUSED_INSIDE[error]) as raised:
        asyncio.run(scenario())
    assert type(raised.value) is error
    assert not m.busy


def test_a_sample_error_reaches_the_await(machines: Machines) -> None:
    m = machines()

    def failing() -> None:
        raise KeyError("sampled")

    async def scenario() -> None:
        await m.sample(failing)

    with pytest.raises(KeyError, match="sampled"):
        asyncio.run(scenario())
    assert not m.busy


SAMPLED: contextvars.ContextVar[str] = contextvars.ContextVar("SAMPLED", default="unset")


def test_sample_runs_in_the_context_of_its_call(machines: Machines) -> None:
    m = machines()

    async def scenario() -> str:
        SAMPLED.set("caller")
        return await m.sample(SAMPLED.get)

    assert asyncio.run(scenario()) == "caller"


def test_a_context_variable_set_inside_a_sample_stays_in_its_copy(machines: Machines) -> None:
    m = machines()

    def setting() -> str:
        SAMPLED.set("inside")
        return SAMPLED.get()

    async def scenario() -> tuple[str, str]:
        SAMPLED.set("caller")
        inside = await m.sample(setting)
        return inside, SAMPLED.get()

    assert asyncio.run(scenario()) == ("inside", "caller")


def test_a_blocking_wait_inside_a_parked_sample_raises_machine_running(
    machines: Machines,
) -> None:
    m = machines()
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    later = handle.submit(lambda: None, (m.time + ms(1)).ps)

    async def scenario() -> object:
        return await m.sample(lambda: later.result(5.0))

    try:
        with pytest.raises(popcorn.MachineRunning, match="cannot wait for a machine request"):
            run_bounded(scenario(), 5)
        assert not m.busy
    finally:
        later.cancel()


def stamped_and_dropped(machines: Machines) -> weakref.ref[popcorn.Machine]:
    """A parked machine left with a sample stamped past its time, and a weak reference to it."""
    m = machines()
    m.sample(lambda: m.time, at=m.time + ms(10))
    return weakref.ref(m)


def test_a_stamped_sample_left_queued_keeps_no_machine_alive(machines: Machines) -> None:
    gone = stamped_and_dropped(machines)
    gc.collect()
    assert gone() is None


def failed_in_a_parked_sample(machines: Machines) -> weakref.ref[popcorn.Machine]:
    """A parked machine whose sample raised, the error kept by the caller's frame."""
    m = machines()

    def failing() -> None:
        raise KeyError(m.time)

    async def scenario() -> None:
        await m.sample(failing)

    with pytest.raises(KeyError):
        asyncio.run(scenario())
    return weakref.ref(m)


def test_a_failed_parked_sample_keeps_no_machine_alive(machines: Machines) -> None:
    gone = failed_in_a_parked_sample(machines)
    gc.collect()
    assert gone() is None


class Captured:
    """What a sampled callable captures, to see whether a withdrawn sample keeps it."""


def returning(held: Captured) -> Callable[[], Captured]:
    """A callable returning `held`."""
    return lambda: held


def withdrawn_samples(m: popcorn.Machine, count: int) -> weakref.ref[Captured]:
    """Post `count` samples stamped far ahead, each withdrawn by a timed-out await, and return
    a weak reference to what the first one captured."""
    first = Captured()
    gone = weakref.ref(first)

    async def scenario() -> None:
        captured: Captured | None = first
        for _ in range(count):
            held = captured or Captured()
            captured = None
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(m.sample(returning(held), at=m.time + s(10)), 0)

    asyncio.run(scenario())
    return gone


def test_withdrawn_samples_keep_a_bounded_number_of_handlers(machines: Machines) -> None:
    m = machines()
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    handlers = len(table)
    gone = withdrawn_samples(m, 1000)
    gc.collect()
    assert len(table) - handlers < 16
    assert gone() is None

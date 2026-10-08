"""Accesses of a hosted machine: sync parked, refused running, `m.read` and `m.write` over
`m.ref`, and the run control around them."""

from __future__ import annotations

import asyncio
import gc
import weakref
from collections.abc import Callable
from typing import Any

import popcorn
import pytest
from popcorn._refs import Ref
from popcorn.arm import Reg
from popcorn.board import AudioOptions, UsbHostOptions
from popcorn.units import VirtualTime, ms, us

from .conftest import BOARD_ELF, VIEWS_ELF, Machines, run_bounded

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
    "reg-name": (lambda m: m.ref.reg.r0, 0x1234),
    "reg-enum": (lambda m: m.ref.reg[Reg.R1], 0x5678),
    "pc": (lambda m: m.ref.pc, None),
    "instructions": (lambda m: m.ref.instructions, None),
    "pending-events": (lambda m: m.ref.pending_events, None),
    "var-member": (lambda m: m.ref.var("table").count, 9),
    "var-element-float": (lambda m: m.ref.var("table").entries[1].gain, 6.5),
    "var-element-int": (lambda m: m.ref.var("matrix")[1][2], -42),
    "var-bit-field": (lambda m: m.ref.var("table").mode, 3),
    "var-signed-bit-field": (lambda m: m.ref.var("wide").high, -100),
    "var-bool": (lambda m: m.ref.var("armed"), False),
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

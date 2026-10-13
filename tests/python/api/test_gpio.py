"""GPIO pins through the GPIO model, FR-PER-7."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Iterable
from typing import cast

import popcorn
import pytest
from popcorn.board import PinMode
from popcorn.units import VirtualTime, us

from .conftest import run_bounded, stalled


def settle(m: popcorn.Machine) -> None:
    asyncio.run(m.run(for_=us(20)))


def test_driving_an_input_reaches_the_firmware_and_its_output(board: popcorn.Machine) -> None:
    pa0, pa5 = board.periph.GPIOA.pin(0), board.periph.GPIOA.pin(5)
    pa0.level = 1
    settle(board)
    assert board.mem.u32[board.sym("pin_seen")] == 1
    assert pa0.level and pa5.level
    pa0.level = 0
    settle(board)
    assert not pa5.level
    assert board.mem.u32[board.sym("pin_seen")] == 0


def test_modes_and_alternate_functions_read_back(board: popcorn.Machine) -> None:
    gpioa = board.periph.GPIOA
    assert (gpioa.pin(0).mode, gpioa.pin(5).mode, gpioa.pin(1).mode) == (
        PinMode.INPUT,
        PinMode.OUTPUT,
        PinMode.ANALOG,
    )
    assert gpioa.pin(5).alternate == 0


def test_a_released_pin_falls_back_to_its_pull(board: popcorn.Machine) -> None:
    pa0 = board.periph.GPIOA.pin(0)
    pa0.level = 1
    assert pa0.level
    pa0.release()
    assert not pa0.level


def test_pins_are_0_to_15_on_gpio_ports(board: popcorn.Machine) -> None:
    with pytest.raises(popcorn.ConfigError):
        board.periph.GPIOA.pin(16)
    with pytest.raises(popcorn.ModelError):
        _ = board.periph.USART1.pin(0).level


class PinSeen:
    def __init__(self, at: int) -> None:
        self.at = at

    def ready(self, machine: popcorn.Machine) -> bool:
        return machine.mem.u32[self.at] == 1


def test_a_drive_inside_running_reaches_the_firmware(board: popcorn.Machine) -> None:
    level = board.ref.periph.GPIOA.pin(0).level

    async def body() -> None:
        async with board.running():
            await board.write((level, True))
            await board.wait_for(PinSeen(board.sym("pin_seen")))

    asyncio.run(body())
    board.periph.GPIOA.pin(0).release()
    assert board.mem.u32[board.sym("pin_seen")] == 1

    async def read() -> bool:
        async with board.running():
            await board.write((level, True))
            (driven,) = await board.read(level)
            return driven

    assert asyncio.run(read())
    board.periph.GPIOA.pin(0).release()


def test_a_drive_survives_a_reset_and_a_snapshot_restores_it(board: popcorn.Machine) -> None:
    pa0 = board.periph.GPIOA.pin(0)
    pa0.level = 1
    saved = board.snapshot()
    board.reset()
    assert pa0.mode is PinMode.ANALOG and not pa0.level
    asyncio.run(board.run(until=board.sym("ready"), budget=1_000_000))
    assert pa0.mode is PinMode.INPUT and pa0.level
    pa0.release()
    assert not pa0.level
    board.restore(saved)
    assert pa0.level


def test_a_drive_from_another_thread_during_a_run_is_refused(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)

    async def body() -> None:
        async with board.running():
            await asyncio.to_thread(setattr, pin, "level", 1)

    with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
        asyncio.run(body())
    asyncio.run(asyncio.to_thread(setattr, pin, "level", 1))
    assert pin.level


EDGE_GAP = us(100)


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def pulse_train(start: VirtualTime) -> list[tuple[VirtualTime, bool]]:
    return [(start + EDGE_GAP * (index + 1), index % 2 == 0) for index in range(6)]


def post_samples(
    m: popcorn.Machine, edges: list[tuple[VirtualTime, bool]]
) -> list[Awaitable[Iterable[object]]]:
    pa5 = m.ref.periph.GPIOA.pin(5).level
    seen = m.ref.mem.u32[m.sym("pin_seen")]
    return [m.read(pa5, seen, at=at + EDGE_GAP // 2) for at, _ in edges]


def test_timed_edges_give_the_same_samples_parked_and_running(board: popcorn.Machine) -> None:
    rerun = popcorn.Machine(board.description, image=board.images[0])
    rerun.reset()
    asyncio.run(rerun.run(until=rerun.sym("ready"), budget=1_000_000))
    assert rerun.time == board.time
    edges: list[tuple[VirtualTime, bool]] = []

    async def running() -> list[tuple[object, ...]]:
        reads: list[Awaitable[Iterable[object]]] = []
        async with rerun.running():

            async def post(boundary: VirtualTime) -> VirtualTime:
                edges.extend(pulse_train(boundary + us(50)))
                pending = rerun.periph.GPIOA.pin(0).drive_edges(edges)
                reads.extend(post_samples(rerun, edges))
                return await pending

            boundary, ran = await stalled(rerun, post)
            assert ran == boundary
            return [tuple(await read) for read in reads]

    running_samples = run_bounded(running(), 30)
    assert running_samples == [(level, int(level)) for _, level in edges]

    posted = asyncio.run(board.periph.GPIOA.pin(0).drive_edges(edges))
    assert posted == board.time

    async def parked() -> list[tuple[object, ...]]:
        reads = post_samples(board, edges)
        await board.run(for_=edges[-1][0] + EDGE_GAP - board.time)
        return [tuple(await read) for read in reads]

    assert run_bounded(parked(), 30) == running_samples
    assert board.periph.GPIOA.pin(0).level is False


def test_an_unsorted_or_past_edge_list_posts_nothing(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)
    events = board.pending_events
    later = board.time + us(10)
    with pytest.raises(ValueError, match="sorted"):
        pin.drive_edges([(later + us(1), True), (later, False)])
    with pytest.raises(ValueError, match="before the machine time"):
        asyncio.run(pin.drive_edges([(VirtualTime(0), True), (later, False)]))
    assert board.pending_events == events
    assert not pin.level


@pytest.mark.parametrize(
    ("edges", "error"),
    [
        ("ab", TypeError),
        (5, TypeError),
        ([(5, True)], TypeError),
        ([(VirtualTime(5), 1)], TypeError),
        ([(VirtualTime(5),)], TypeError),
        ([], ValueError),
        ([(VirtualTime(-1), True)], ValueError),
        ([(VirtualTime(1 << 64), True)], ValueError),
    ],
    ids=["str", "int", "time-type", "level-type", "short", "empty", "negative", "overflow"],
)
def test_a_bad_edge_list_is_refused_at_the_call(
    board: popcorn.Machine, edges: object, error: type[Exception]
) -> None:
    events = board.pending_events
    with pytest.raises(error):
        board.periph.GPIOA.pin(0).drive_edges(cast(list[tuple[VirtualTime, bool]], edges))
    assert board.pending_events == events


def test_more_than_65536_edges_are_refused_whole(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)
    start = board.time + us(1)
    edges = [(start + us(index), index % 2 == 0) for index in range(65536)]
    with pytest.raises(ValueError, match="65536"):
        pin.drive_edges([*edges, (start + us(70000), True)])
    events = board.pending_events
    asyncio.run(pin.drive_edges(edges[:-1]))
    assert board.pending_events == events + 65535
    last = start + us(70000)
    with pytest.raises(popcorn.ConfigError, match="pending pin edges"):
        asyncio.run(pin.drive_edges([(last, True), (last, False)]))
    assert board.pending_events == events + 65535
    asyncio.run(pin.drive_at(True, at=last))
    with pytest.raises(popcorn.ConfigError, match="pending pin edges"):
        asyncio.run(pin.drive_at(False, at=last))
    assert board.pending_events == events + 65536


def test_a_thousand_edges_then_a_restore_leave_the_snapshot_events(
    board: popcorn.Machine,
) -> None:
    pin = board.periph.GPIOA.pin(0)
    saved = board.snapshot()
    events = board.pending_events
    for index in range(1000):
        asyncio.run(pin.drive_at(index % 2 == 0, at=board.time + us(index + 1)))
    assert board.pending_events == events + 1000
    board.restore(saved)
    assert board.pending_events == events
    settle(board)
    assert not pin.level


def test_a_snapshot_keeps_pending_edges(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)
    asyncio.run(pin.drive_at(True, at=board.time + us(10)))
    saved = board.snapshot()
    settle(board)
    assert pin.level
    pin.level = False
    board.restore(saved)
    assert not pin.level
    settle(board)
    assert pin.level
    assert board.mem.u32[board.sym("pin_seen")] == 1


def test_edges_take_no_gil_on_the_machine_thread(board: popcorn.Machine) -> None:
    asyncio.run(board.periph.GPIOA.pin(0).drive_edges(pulse_train(board.time)))
    before = gil_entries(board)
    asyncio.run(board.run(for_=EDGE_GAP * 8))
    assert gil_entries(board) == before


def test_an_edge_list_ending_past_the_range_after_a_system_reset_schedules_nothing(
    board: popcorn.Machine,
) -> None:
    settle(board)
    board.reset(popcorn.ResetKind.SOFTWARE)
    pin = board.periph.GPIOA.pin(0)
    events = board.pending_events
    with pytest.raises(ValueError, match="64-bit"):
        asyncio.run(
            pin.drive_edges([(board.time + us(1), True), (VirtualTime((1 << 64) - 1), False)])
        )
    assert board.pending_events == events


def test_a_power_on_reset_keeps_a_pending_edge_at_its_instant(board: popcorn.Machine) -> None:
    settle(board)
    board.reset(popcorn.ResetKind.SOFTWARE)
    asyncio.run(board.run(until=board.sym("ready"), budget=1_000_000))
    pin = board.periph.GPIOA.pin(0)
    due = board.time + us(5000)
    asyncio.run(pin.drive_at(True, at=due))
    asyncio.run(board.run(for_=us(1000)))
    reset_at = board.time
    board.reset()
    asyncio.run(board.run(until=board.sym("ready"), budget=1_000_000))
    after = VirtualTime(due.ps - reset_at.ps)
    assert board.time < after
    asyncio.run(board.run(for_=after - board.time - us(50)))
    assert not pin.level
    asyncio.run(board.run(for_=us(100)))
    assert pin.level

"""Timer inputs driven through GPIO pins in alternate function mode, the tim_ll firmware."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn.units import VirtualTime, us

from .conftest import run_bounded, stalled

TIM_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "tim_ll.elf"
EDGE_GAP = us(20)


def entered(u575: popcorn.PartDescription, entry: str) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(TIM_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym(entry)
    return m


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    return [m.mem.u32[m.sym("log_buf") + 4 * index] for index in range(count)]


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def quadrature(
    start: VirtualTime, forward: int, back: int
) -> dict[int, list[tuple[VirtualTime, bool]]]:
    levels = {6: False, 7: False}
    edges: dict[int, list[tuple[VirtualTime, bool]]] = {6: [], 7: []}
    at = start
    for step in range(forward + back):
        ahead = step < forward
        pin = (6 if ahead else 7) if levels[6] == levels[7] else (7 if ahead else 6)
        levels[pin] = not levels[pin]
        at = at + EDGE_GAP
        edges[pin].append((at, levels[pin]))
    return edges


def drive(m: popcorn.Machine, edges: dict[int, list[tuple[VirtualTime, bool]]]) -> None:
    for pin, pin_edges in edges.items():
        asyncio.run(m.periph.GPIOA.pin(pin).drive_edges(pin_edges))


def test_an_encoder_counts_a_quadrature_sequence_alike_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    running = entered(u575, "tim3_encoder")
    edges: dict[int, list[tuple[VirtualTime, bool]]] = {}

    async def body() -> None:
        async with running.running():

            async def post(boundary: VirtualTime) -> None:
                edges.update(quadrature(boundary + us(200), 40, 10))
                posted = [
                    running.periph.GPIOA.pin(pin).drive_edges(pin_edges)
                    for pin, pin_edges in edges.items()
                ]
                for pending in posted:
                    await pending

            await stalled(running, post)
            await running.wait_for(running.ref.mem.u32[running.sym("log_len")].ge(3))

    with pytest.raises(popcorn.EmulationError, match="USER"):
        run_bounded(body(), 10)
    assert log(running) == [0, 30, 1]

    m = entered(u575, "tim3_encoder")
    drive(m, edges)
    before = gil_entries(m)
    stop = asyncio.run(m.run(budget=50_000_000))
    assert stop.reason is popcorn.StopReason.USER
    assert gil_entries(m) == before
    assert m.periph.TIM3.CNT.read() == 30
    assert (log(m), m.time) == (log(running), running.time)


def pulse_train(start: VirtualTime, count: int) -> list[tuple[VirtualTime, bool]]:
    edges: list[tuple[VirtualTime, bool]] = []
    for pulse in range(count):
        edges.append((start + EDGE_GAP * (2 * pulse + 1), True))
        edges.append((start + EDGE_GAP * (2 * pulse + 2), False))
    return edges


def test_a_capture_with_icpsc_4_and_a_filter_logs_every_fourth_edge_alike_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    running = entered(u575, "tim3_capture")
    edges: list[tuple[VirtualTime, bool]] = []

    async def body() -> None:
        async with running.running():

            async def post(boundary: VirtualTime) -> VirtualTime:
                edges.extend(pulse_train(boundary + us(200), 12))
                return await running.periph.GPIOA.pin(6).drive_edges(edges)

            await stalled(running, post)
            await running.wait_for(running.ref.mem.u32[running.sym("log_len")].ge(4))

    with pytest.raises(popcorn.EmulationError, match="USER"):
        run_bounded(body(), 10)
    first, second, third, overcapture = log(running)
    ticks_per_capture = 4 * 2 * EDGE_GAP.ps // 250_000
    assert (second - first, third - second, overcapture) == (ticks_per_capture,) * 2 + (0,)

    m = entered(u575, "tim3_capture")
    asyncio.run(m.periph.GPIOA.pin(6).drive_edges(edges))
    stop = asyncio.run(m.run(budget=50_000_000))
    assert stop.reason is popcorn.StopReason.USER
    assert (log(m), m.time) == (log(running), running.time)

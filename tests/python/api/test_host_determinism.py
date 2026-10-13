"""Rule 3 of the machine host: the same code from the same state gives the same virtual results,
whatever the wall timing of the test thread and of other threads."""

from __future__ import annotations

import asyncio
import hashlib
import random
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass

import popcorn
from popcorn.units import VirtualTime, ms, us

from .conftest import BOARD_ELF


@dataclass(frozen=True)
class Digest:
    instructions: int
    time: VirtualTime
    registers: tuple[int, ...]
    ram: str
    ring: str
    frames: tuple[tuple[int, int], ...]


class Echoed:
    """Ready once the firmware echoed `count` bytes."""

    def __init__(self, echoed: popcorn.Var, count: int) -> None:
        self.echoed = echoed
        self.count = count

    def ready(self, machine: popcorn.Machine) -> bool:
        return self.echoed.value >= self.count


def scenario(
    u575: popcorn.PartDescription, noise: Callable[[], None], read_count: int = 0
) -> Digest:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    reading = threading.Event()
    stop_reading = threading.Event()

    def reader() -> None:
        async def reads() -> None:
            for _ in range(read_count):
                if stop_reading.is_set():
                    return
                await m.read(m.ref.instructions)
                await asyncio.sleep(0.0001)

        reading.set()
        asyncio.run(reads())

    async def body() -> tuple[tuple[int, int], ...]:
        await m.run(until="ready", budget=1_000_000)
        noise()
        port = m.periph.USART1.port
        written = port.write(b"ping", at=m.time + us(50))
        await m.wait_for(Echoed(m.var("echoed"), 4))
        await written
        noise()
        called = await m.call("ready")
        assert called.stop.reason is popcorn.StopReason.RETURNED
        await m.run(for_=ms(100))
        frames = await port.read_frames(port.available)
        return tuple((frame.data, frame.at.ps) for frame in frames)

    third = threading.Thread(target=reader)
    third.start()
    reading.wait()
    try:
        frames = asyncio.run(body())
    finally:
        stop_reading.set()
        third.join()
    ram = hashlib.sha256()
    for region in m.description.regions:
        if region.name.startswith("SRAM"):
            ram.update(m.mem.read(region.start, region.size))
    return Digest(
        m.instructions,
        m.time,
        tuple(getattr(m.reg, name) for name in m.reg.names()),
        ram.hexdigest(),
        m.ring.dump(),
        frames,
    )


def test_two_runs_with_different_wall_timing_give_the_same_virtual_results(
    u575: popcorn.PartDescription,
) -> None:
    quiet = scenario(u575, lambda: None)
    spinning = threading.Event()

    def spin() -> None:
        while not spinning.is_set():
            pass

    spinner = threading.Thread(target=spin)
    spinner.start()
    try:
        noisy = scenario(u575, lambda: time.sleep(random.uniform(0, 0.005)), read_count=1000)
    finally:
        spinning.set()
        spinner.join()
    assert len(quiet.frames) == 4
    assert noisy == quiet

"""Members of linked systems built from the usart_ll firmware, for the system tests."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Mapping
from pathlib import Path

import popcorn
import popcorn.model
import pytest
from popcorn import StopReason, System, UartLink
from popcorn.units import VirtualDuration, VirtualTime, us

USART_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "usart_ll.elf"
LINK_BYTE = 1
PING_BYTES = 16


def member(
    u575: popcorn.PartDescription,
    entry: str,
    name: str,
    slice_instructions: int = 1_000_000,
    *,
    strict: bool = False,
    slice_wall_budget: float = 1e-3,
    models: Mapping[str, Callable[[], popcorn.model.Model]] | None = None,
) -> popcorn.Machine:
    """A usart_ll machine named `name`, stopped at main with pc at `entry`."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(USART_LL),
        name=name,
        slice_instructions=slice_instructions,
        strict_peripherals=strict,
        slice_wall_budget=slice_wall_budget,
        models=models,
    )
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    m.reg.pc = m.sym(entry)
    return m


def logged(m: popcorn.Machine) -> list[int]:
    """The firmware's log words."""
    count = m.mem.u32[m.sym("log_len")]
    base = m.sym("log_buf")
    return [m.mem.u32[base + 4 * index] for index in range(min(count, 256))]


def received(m: popcorn.Machine) -> list[int]:
    """The bytes the firmware's link interrupt logged."""
    return [word & 0xFFFF for word in logged(m)[::2] if word >> 16 == LINK_BYTE]


class Ticker(popcorn.model.Model):
    """A TIM6 model whose Python method runs every `period_us` of virtual time from its reset,
    counting `ticks`, and raising RuntimeError at tick `raise_at` when that is above 0."""

    ticks: int = 0
    raise_at: int = 0
    period_us: int = 10

    def reset(self, kind: popcorn.ResetKind) -> None:
        super().reset(kind)
        self.schedule(us(self.period_us), self._tick)

    def _tick(self) -> None:
        self.ticks += 1
        if self.ticks == self.raise_at:
            raise RuntimeError(f"{self.peripheral} tick {self.ticks}")
        self.schedule(us(self.period_us), self._tick)


def ticker(m: popcorn.Machine) -> Ticker:
    """`m`'s TIM6 model, a Ticker."""
    model = m.model("TIM6")
    assert isinstance(model, Ticker)
    return model


L = us(20)
VARIANT_VALUES: list[tuple[int, VirtualDuration | None]] = [
    (1_000_000, None),
    (997, None),
    (1_000_000, us(3)),
    (997, us(3)),
]
VARIANTS = [
    pytest.param(*values, id=name)
    for values, name in zip(VARIANT_VALUES, ("plain", "997", "q=3us", "997 q=3us"), strict=True)
]
TX_BYTES = [0x20 + index for index in range(64)]
FIRMWARE_BAUD = 461_095

Build = Callable[..., System]


def links(third: VirtualDuration | None, baud: int = 460_800) -> list[UartLink]:
    made = [UartLink("a.USART1", "b.USART1", latency=L, baud=baud)]
    if third is not None:
        made.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
    return made


async def to(system: System, until: VirtualTime) -> None:
    """Run to `until`, again after each member stop."""
    while (await system.run(until=until)).reason is not StopReason.TIME:
        pass


def past_done(system: System) -> None:
    """Move a, parked on link_tx's final breakpoint, to the idle loop, so a free run goes on."""
    a = system.members["a"]
    assert a.mem.u16[a.pc - 2] == 0xBE01
    a.reg.pc = a.sym("idle")


def at(micros: float) -> VirtualTime:
    return VirtualTime(us(micros).ps)


async def in_flight(system: System) -> None:
    """Let the event loop turn until the system's free run resumes after a member wait."""
    for _ in range(100):
        if system._flight is not None:  # pyright: ignore[reportPrivateUsage]
            return
        await asyncio.sleep(0)
    raise AssertionError("the free run did not resume")

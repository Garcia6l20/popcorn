"""The machine-side query and watch of the clock the RCC drives onto a GPIO pin."""

from __future__ import annotations

import asyncio
import weakref
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, StopReason
from popcorn._hooks import Relay  # pyright: ignore[reportPrivateUsage]
from popcorn.units import us

from .conftest import collected

RCC_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
RCC_BDCR = 0x4602_0CF0
LSION = 1 << 26
LSCOEN = 1 << 24


def parked(u575: popcorn.PartDescription) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    return m


def test_the_lsco_pin_reads_the_lsi_rate_once_enabled(u575: popcorn.PartDescription) -> None:
    m = parked(u575)
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    assert handle.gpio_clock("GPIOA", 2) == 0
    m.mem.u32[RCC_BDCR] = LSION | LSCOEN
    asyncio.run(m.run(for_=us(300)))
    assert handle.gpio_clock("GPIOA", 2) == 32_000
    assert handle.gpio_clock("GPIOA", 3) == 0
    m.mem.u32[RCC_BDCR] = LSION
    assert handle.gpio_clock("GPIOA", 2) == 0


def test_a_pin_the_part_does_not_have_is_refused(u575: popcorn.PartDescription) -> None:
    m = parked(u575)
    with pytest.raises(ConfigError):
        m._handle.gpio_clock("GPIOA", 16)  # pyright: ignore[reportPrivateUsage]


def test_a_watch_is_called_once_per_rate_change_and_ends_when_removed(
    u575: popcorn.PartDescription,
) -> None:
    m = parked(u575)
    calls: list[tuple[int, int]] = []
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]

    def record(hz: int, at: int) -> None:
        calls.append((hz, at))

    key = table.add(record, in_caller=True)
    watch = m._handle.gpio_clock_watch(  # pyright: ignore[reportPrivateUsage]
        "GPIOA", 2, Relay(table, key, quiet=True)
    )
    m.mem.u32[RCC_BDCR] = LSION
    asyncio.run(m.run(for_=us(300)))
    assert calls == []
    start = m.time.ps
    m.mem.u32[RCC_BDCR] = LSION | LSCOEN
    m.mem.u32[RCC_BDCR] = LSION | LSCOEN
    asyncio.run(m.run(for_=us(10)))
    assert calls == [(32_000, start)]
    watch.remove()
    assert not watch.active()
    m.mem.u32[RCC_BDCR] = LSION
    asyncio.run(m.run(for_=us(10)))
    assert calls == [(32_000, start)]


class Captured:
    pass


def test_a_clock_watch_keeps_no_machine_or_callable_alive(u575: popcorn.PartDescription) -> None:
    m = parked(u575)
    captured = Captured()
    seen: list[int] = []

    def record(hz: int, at: int, captured: Captured = captured) -> None:
        seen.append(hz)

    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    key = table.add(record, in_caller=True)
    watch = m._handle.gpio_clock_watch(  # pyright: ignore[reportPrivateUsage]
        "GPIOA", 2, Relay(table, key, quiet=True)
    )
    watched = weakref.ref(captured)
    machine = weakref.ref(m)
    del captured, record
    m.mem.u32[RCC_BDCR] = LSION | LSCOEN
    asyncio.run(m.run(for_=us(300)))
    assert seen == [32_000]
    del m, table
    assert collected(machine)
    assert collected(watched)
    assert not watch.active()
    watch.remove()
    watch.remove()


def test_a_pin_watch_handle_is_inactive_and_removes_silently_once_its_machine_is_gone(
    u575: popcorn.PartDescription,
) -> None:
    m = parked(u575)
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    handle = m._handle  # pyright: ignore[reportPrivateUsage]
    keys = [table.add(lambda *args: None, in_caller=True) for _ in range(4)]
    level_watch = handle.gpio_watch("GPIOA", 2, Relay(table, keys[0], quiet=True))
    clock_watch = handle.gpio_clock_watch("GPIOA", 2, Relay(table, keys[1], quiet=True))
    live_level = handle.gpio_watch("GPIOA", 3, Relay(table, keys[2], quiet=True))
    live_clock = handle.gpio_clock_watch("GPIOA", 3, Relay(table, keys[3], quiet=True))
    assert level_watch.active() and clock_watch.active()
    live_level.remove()
    live_clock.remove()
    assert not live_level.active() and not live_clock.active()
    machine = weakref.ref(m)
    del m, table, handle
    assert collected(machine)
    for gone in (level_watch, clock_watch):
        assert not gone.active()
        gone.remove()
        gone.remove()
        assert not gone.active()

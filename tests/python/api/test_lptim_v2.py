"""The STM32U5 LPTIM: every instance has a model, and the LL firmware's matches come at the same
virtual times parked and running."""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, support
from popcorn.units import VirtualTime, ms, ns, us

from .conftest import run_bounded

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "lptim_ll.elf"
RCC_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
U5_PARTS = ("STM32U575QGIx", "STM32U585AIIx")
INSTANCES = ("LPTIM1", "LPTIM2", "LPTIM3", "LPTIM4")
PERIODS = 10


@pytest.mark.parametrize("part", U5_PARTS)
def test_every_u5_lptim_has_a_builtin_model(part: str) -> None:
    models = {p.name: p.model for p in support(mcu(part))}
    assert [models[name] for name in INSTANCES] == ["builtin"] * len(INSTANCES)


def matches_of(m: popcorn.Machine) -> list[tuple[str, int]]:
    seen: list[tuple[str, int]] = []

    def recorder(name: str) -> Callable[[popcorn.Call], popcorn.Return]:
        def record(call: popcorn.Call) -> popcorn.Return:
            seen.append((name, m.time.ps))
            return call.proceed()

        return record

    m.intercept("on_compare", recorder("compare"))
    m.intercept("on_reload", recorder("reload"))
    return seen


def booted() -> popcorn.Machine:
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(PREBUILT)]).machine()
    m.reset()
    m.reg.pc = m.sym("lptim1_forever")
    return m


def parked(m: popcorn.Machine) -> popcorn.Stop:
    return asyncio.run(m.run(for_=ms(5)))


def running(m: popcorn.Machine) -> popcorn.Stop:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("reloads").ge(PERIODS))

    return run_bounded(body(), 20)


def test_the_matches_are_equal_parked_and_running() -> None:
    results: list[list[tuple[str, int]]] = []
    for run in (parked, running):
        m = booted()
        seen = matches_of(m)
        stop = run(m)
        assert stop.reason in (popcorn.StopReason.TIME, popcorn.StopReason.USER), stop
        assert m.var("reloads").value >= PERIODS
        results.append(seen[: 2 * PERIODS])
    assert results[0] == results[1]
    assert [name for name, _ in results[0]] == ["compare", "reload"] * PERIODS


async def test_ue_sets_at_every_third_overflow_with_rep_2() -> None:
    m = booted()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    timer = m.periph.LPTIM1
    timer.CR.write(1)
    timer.RCR.write(2)
    timer.ARR.write(9)
    await m.run(for_=us(5))
    timer.CR.write(1 | 1 << 2)
    await m.run(for_=us(8))
    assert timer.ISR_output.UE == 0
    await m.run(for_=us(0.5))
    assert timer.ISR_output.UE == 1
    assert timer.ISR_output.REPOK == 1


CONVERSION = us(1.5) + us(0.75)
CPU_CYCLE = ns(250)


async def started_lptim_with_a_listening_dac() -> tuple[popcorn.Machine, VirtualTime]:
    """A DAC1 channel 1 listening to `lptim1_ch1` and an LPTIM1 whose CNTSTRT was just written."""
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(RCC_LL)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    m.periph.RCC.AHB3ENR.DAC1EN = 1
    timer = m.periph.LPTIM1
    timer.CCMR1.write(1 << 1)
    timer.CR.write(1)
    timer.ARR.write(5)
    await m.run(for_=us(1))
    timer.CCR1.write(2)
    await m.run(for_=us(1))
    timer.ICR_output.write(0x18)
    dac = m.periph.DAC1
    dac.CR.write(1 << 1 | 11 << 2)
    dac.DHR12R1.write(0x2A5)
    started = m.time
    timer.CR.write(1 | 1 << 2)
    return m, started


@pytest.mark.parametrize("mode", ["parked", "running"])
async def test_a_dac_conversion_triggered_by_lptim1_ch1_comes_at_the_same_virtual_time(
    mode: str,
) -> None:
    m, started = await started_lptim_with_a_listening_dac()
    dac = m.periph.DAC1

    def read() -> tuple[int, int]:
        return m.time.ps, dac.DOR1.read() & 0xFFF

    due = started + CONVERSION
    samples = [m.sample(read, at=at) for at in (due - CPU_CYCLE, due, due + CPU_CYCLE)]
    if mode == "parked":
        await m.run(for_=us(10))
        seen = [await one for one in samples]
    else:
        async with m.running():
            seen = [await one for one in samples]
    assert seen == [
        ((due - CPU_CYCLE).ps, 0),
        (due.ps, 0x2A5),
        ((due + CPU_CYCLE).ps, 0x2A5),
    ]


async def started_lptim_counting_pb5() -> tuple[popcorn.Machine, VirtualTime]:
    """An LPTIM1 with COUNTMODE 1, CCR1 3, input 1 on PB5 AF1, whose CNTSTRT was just written."""
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(RCC_LL)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    m.periph.RCC.AHB2ENR1.GPIOBEN = 1
    gpiob = m.periph.GPIOB
    gpiob.MODER.write(gpiob.MODER.read() & ~(3 << 10) | 2 << 10)
    gpiob.AFRL.write(gpiob.AFRL.read() & ~(15 << 20) | 1 << 20)
    timer = m.periph.LPTIM1
    timer.CFGR.write(1 << 23)
    timer.CR.write(1)
    timer.ARR.write(100)
    await m.run(for_=us(1))
    timer.CCR1.write(3)
    await m.run(for_=us(1))
    timer.ICR_output.write(0x18)
    started = m.time
    timer.CR.write(1 | 1 << 2)
    return m, started


@pytest.mark.parametrize("mode", ["parked", "running"])
async def test_pb5_edges_counted_by_countmode_come_at_the_same_virtual_time(mode: str) -> None:
    m, started = await started_lptim_counting_pb5()
    timer = m.periph.LPTIM1
    rises = [started + us(10) + us(2.5) * index for index in range(5)]
    edges = [edge for at in rises for edge in ((at, True), (at + us(1.25), False))]
    await m.periph.GPIOB.pin(5).drive_edges(edges)

    def read() -> tuple[int, int, int]:
        return m.time.ps, timer.CNT.read(), timer.ISR_output.CC1IF

    samples = [m.sample(read, at=at) for at in (rises[3] - CPU_CYCLE, rises[3], rises[4])]
    if mode == "parked":
        await m.run(for_=us(30))
        seen = [await one for one in samples]
    else:
        async with m.running():
            seen = [await one for one in samples]
    assert seen == [
        ((rises[3] - CPU_CYCLE).ps, 3, 0),
        (rises[3].ps, 4, 1),
        (rises[4].ps, 5, 1),
    ]


async def started_lptim_encoding_pb5_pb7() -> tuple[popcorn.Machine, VirtualTime]:
    """An LPTIM1 with ENC 1 on both edges, input 1 on PB5 and input 2 on PB7 AF1, whose CNTSTRT
    was just written."""
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(RCC_LL)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    m.periph.RCC.AHB2ENR1.GPIOBEN = 1
    gpiob = m.periph.GPIOB
    gpiob.MODER.write(gpiob.MODER.read() & ~(3 << 10 | 3 << 14) | 2 << 10 | 2 << 14)
    gpiob.AFRL.write(gpiob.AFRL.read() & ~(15 << 20 | 15 << 28) | 1 << 20 | 1 << 28)
    timer = m.periph.LPTIM1
    timer.CFGR.write(1 << 24 | 2 << 1)
    timer.CR.write(1)
    timer.ARR.write(100)
    await m.run(for_=us(1))
    timer.ICR_output.write(0x10)
    started = m.time
    timer.CR.write(1 | 1 << 2)
    return m, started


@pytest.mark.parametrize("mode", ["parked", "running"])
async def test_encoder_edges_on_pb5_and_pb7_count_at_the_same_virtual_time(mode: str) -> None:
    m, started = await started_lptim_encoding_pb5_pb7()
    timer = m.periph.LPTIM1
    steps = [(5, True), (7, True), (7, False), (5, False), (7, True)]
    at = [started + us(10) + us(2.5) * index for index in range(len(steps))]
    for pin in (5, 7):
        await m.periph.GPIOB.pin(pin).drive_edges(
            [(when, level) for when, (one, level) in zip(at, steps, strict=True) if one == pin]
        )

    def read() -> tuple[int, int, int]:
        return m.time.ps, timer.CNT.read(), timer.ISR_output.DOWN

    samples = [m.sample(read, at=when) for when in (at[1], at[2] - CPU_CYCLE, at[2], at[4])]
    if mode == "parked":
        await m.run(for_=us(30))
        seen = [await one for one in samples]
    else:
        async with m.running():
            seen = [await one for one in samples]
    assert seen == [
        (at[1].ps, 2, 0),
        ((at[2] - CPU_CYCLE).ps, 2, 0),
        (at[2].ps, 1, 1),
        (at[4].ps, 100, 1),
    ]


async def started_lptim_capturing_pb2() -> tuple[popcorn.Machine, VirtualTime]:
    """An LPTIM1 with channel 1 capturing rising edges of PB2 AF1, whose CNTSTRT was just
    written."""
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(RCC_LL)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB3ENR.LPTIM1EN = 1
    m.periph.RCC.AHB2ENR1.GPIOBEN = 1
    gpiob = m.periph.GPIOB
    gpiob.MODER.write(gpiob.MODER.read() & ~(3 << 4) | 2 << 4)
    gpiob.AFRL.write(gpiob.AFRL.read() & ~(15 << 8) | 1 << 8)
    timer = m.periph.LPTIM1
    timer.CCMR1.write(1 | 1 << 1)
    timer.CR.write(1)
    timer.ARR.write(1000)
    await m.run(for_=us(1))
    timer.ICR_output.write(0x10)
    started = m.time
    timer.CR.write(1 | 1 << 2)
    return m, started


@pytest.mark.parametrize("mode", ["parked", "running"])
async def test_pb2_captures_into_ccr1_at_the_same_virtual_time(mode: str) -> None:
    m, started = await started_lptim_capturing_pb2()
    timer = m.periph.LPTIM1
    rises = [started + us(10) + us(5) * index for index in range(3)]
    edges = [edge for at in rises for edge in ((at, True), (at + us(2.5), False))]
    await m.periph.GPIOB.pin(2).drive_edges(edges)

    def read() -> tuple[int, int, int]:
        return m.time.ps, timer.ISR_output.CC1IF, timer.CCR1.read()

    due = rises[2] + us(0.5)
    samples = [m.sample(read, at=at) for at in (due - CPU_CYCLE, due)]
    if mode == "parked":
        await m.run(for_=us(30))
        seen = [await one for one in samples]
    else:
        async with m.running():
            seen = [await one for one in samples]
    assert seen == [
        ((due - CPU_CYCLE).ps, 1, 59),
        (due.ps, 1, 79),
    ]

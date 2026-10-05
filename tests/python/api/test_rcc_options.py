"""The board clock sources of the built-in STM32 RCC model: Machine(rcc=) and [board.RCC]."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, StopReason
from popcorn.board import RccOptions
from popcorn.units import VirtualDuration, ms

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
RCC_ELF = PREBUILT / "rcc_ll.elf"
CYCLES = 16_000
RCC_CR = 0x4602_0C00
RCC_CFGR1 = 0x4602_0C1C
HSEON = 1 << 16
SW = 0b11
SW_HSE = 0b10


def on_hse(m: popcorn.Machine) -> None:
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    m.mem.u32[RCC_CR] |= HSEON
    asyncio.run(m.run(for_=ms(1)))
    assert m.periph.RCC.RCC_CR.HSERDY == 1
    m.mem.u32[RCC_CFGR1] = m.mem.u32[RCC_CFGR1] & ~SW | SW_HSE
    asyncio.run(m.run(for_=ms(1)))
    assert m.periph.RCC.RCC_CFGR1.SWS == SW_HSE


def cycle_time(m: popcorn.Machine) -> VirtualDuration:
    start = m.time
    stop = asyncio.run(m.run(budget=CYCLES))
    assert stop.reason is StopReason.BUDGET
    return m.time - start


def test_the_defaults_mirror_the_rcc_model() -> None:
    assert RccOptions() == RccOptions(hse_hz=16_000_000, lse_hz=32_768)


@pytest.mark.parametrize(
    ("rcc", "hz"),
    [
        (None, 16_000_000),
        (RccOptions(hse_hz=32_000_000), 32_000_000),
        (RccOptions(4_000_000), 4_000_000),
        (RccOptions(50_000_000, lse_hz=1_000_000), 50_000_000),
    ],
)
def test_the_core_runs_at_the_board_hse_rate_once_switched_to_hse(
    u575: popcorn.MachineDescription, rcc: RccOptions | None, hz: int
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), rcc=rcc)
    on_hse(m)
    assert m.instructions > 0
    assert cycle_time(m).ps == CYCLES * 10**12 // hz


@pytest.mark.parametrize(
    ("rcc", "key"),
    [
        (RccOptions(hse_hz=3_999_999), "rcc.hse_hz"),
        (RccOptions(hse_hz=50_000_001), "rcc.hse_hz"),
        (RccOptions(lse_hz=0), "rcc.lse_hz"),
        (RccOptions(lse_hz=32_768_000), "rcc.lse_hz"),
    ],
)
def test_a_clock_outside_its_range_is_refused(
    u575: popcorn.MachineDescription, rcc: RccOptions, key: str
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(u575, rcc=rcc)
    assert caught.value.key == key


def test_board_rcc_in_the_project_sets_the_hse_rate(
    u575: popcorn.MachineDescription,
) -> None:
    project = popcorn.Project(
        part="STM32U575QGIx", images=[popcorn.Elf(RCC_ELF)], rcc=RccOptions(hse_hz=32_000_000)
    )
    m = project.machine()
    on_hse(m)
    assert cycle_time(m).ps == CYCLES * 10**12 // 32_000_000


RCC_CFGR2 = 0x4602_0C20
HPRE_DIV512 = 0xF
CODE = 0x2008_0000
STORE = bytes.fromhex("0160")
NOP = bytes.fromhex("00bf")
HERE = bytes.fromhex("fee7")


NOP_W = bytes.fromhex("aff30080")


def on_main(u575: popcorn.MachineDescription) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.mem.write(CODE + 0x1000, HERE)
    return m


def period_ps(m: popcorn.Machine) -> int:
    m.reg.pc = CODE + 0x1000
    start = m.time
    assert asyncio.run(m.run(budget=100)).reason is StopReason.BUDGET
    return (m.time - start).ps // 100


@pytest.mark.parametrize(("wide", "nops"), [(0, 0), (0, 100), (0, 400), (5, 10)])
def test_the_rest_of_a_block_runs_at_the_rate_its_store_set(
    u575: popcorn.MachineDescription, wide: int, nops: int
) -> None:
    m = on_main(u575)
    old = period_ps(m)
    m.mem.write(CODE, NOP_W * wide + STORE + NOP * nops + HERE)
    m.reg.r0 = RCC_CFGR2
    m.reg.r1 = m.mem.u32[RCC_CFGR2] | HPRE_DIV512
    m.reg.pc = CODE
    start, counted = m.time, m.instructions
    assert asyncio.run(m.run(budget=wide + nops + 1)).reason is StopReason.BUDGET
    assert m.periph.RCC.RCC_CFGR2.HPRE == HPRE_DIV512
    after_store = m.instructions - counted - wide - 1
    elapsed = (m.time - start).ps
    new = period_ps(m)
    assert new > 500 * old
    assert abs(elapsed - ((wide + 1) * old + new * after_store)) <= after_store + 1


def test_a_stepped_store_and_a_debugger_write_charge_no_block_ahead(
    u575: popcorn.MachineDescription,
) -> None:
    m = on_main(u575)
    old = period_ps(m)
    m.mem.write(CODE, STORE + NOP * 20 + HERE)
    m.reg.r0 = CODE + 0x2000
    m.reg.pc = CODE
    assert asyncio.run(m.run(budget=22)).reason is StopReason.BUDGET
    cfgr2 = m.mem.u32[RCC_CFGR2]
    m.reg.r0 = RCC_CFGR2
    m.reg.r1 = cfgr2 | HPRE_DIV512
    m.reg.pc = CODE
    start = m.time
    asyncio.run(m.step())
    assert (m.time - start).ps == old
    assert m.periph.RCC.RCC_CFGR2.HPRE == HPRE_DIV512
    m.reg.r0 = CODE + 0x2000
    m.reg.pc = CODE
    assert asyncio.run(m.run(budget=22)).reason is StopReason.BUDGET
    assert m.pc == CODE + len(STORE + NOP * 20)
    start = m.time
    m.mem.u32[RCC_CFGR2] = cfgr2
    assert m.time == start
    assert period_ps(m) == old


def test_pending_events_counts_the_hse_ready_event_until_it_fires(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    before = m.pending_events
    m.mem.u32[RCC_CR] |= HSEON
    assert m.pending_events == before + 1
    asyncio.run(m.run(for_=ms(1)))
    assert m.periph.RCC.RCC_CR.HSERDY == 1
    assert m.pending_events == before

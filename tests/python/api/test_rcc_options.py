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


def test_board_rcc_in_the_project_file_sets_the_hse_rate(
    tmp_path: Path, u575: popcorn.MachineDescription
) -> None:
    path = tmp_path / "popcorn.toml"
    path.write_text(
        f'[mcu]\npart = "STM32U575QGIx"\n[image.app]\nelf = "{RCC_ELF}"\n'
        "[board.RCC]\nhse_hz = 32_000_000\n"
    )
    m = popcorn.Project.load(path).machine()
    on_hse(m)
    assert cycle_time(m).ps == CYCLES * 10**12 // 32_000_000


@pytest.mark.parametrize(
    ("board", "key"),
    [
        ("[board.RCC]\nhse_hz = 60_000_000\n", "board.RCC.hse_hz"),
        ("[board.RCC]\nlse_hz = 0\n", "board.RCC.lse_hz"),
        ("[board.PWR]\nhse_hz = 8_000_000\n", "board.PWR.hse_hz"),
    ],
)
def test_a_bad_board_clock_names_its_key_and_line(tmp_path: Path, board: str, key: str) -> None:
    path = tmp_path / "popcorn.toml"
    path.write_text(f'[mcu]\npart = "STM32U575QGIx"\n{board}')
    with pytest.raises(ConfigError) as caught:
        popcorn.Project.load(path)
    assert (caught.value.key, caught.value.line) == (key, 4)

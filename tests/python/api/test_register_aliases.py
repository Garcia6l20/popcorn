"""Bare register names on the STM32U575, the SVD-prefixed names kept as aliases."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn.trace import Mmio
from popcorn.units import us

from .conftest import BOARD_ELF

SAME_REGISTER = [
    ("TIM1", "ARR", "TIM1_ARR"),
    ("TIM8", "ARR", "TIM1_ARR"),
    ("RCC", "PLL1DIVR", "RCC_PLL1DIVR"),
    ("EXTI", "IMR1", "EXTI_IMR1"),
    ("GPIOB", "MODER", "GPIO_MODER"),
    ("SEC_GPIOB", "MODER", "GPIO_MODER"),
]


@pytest.mark.parametrize(("peripheral", "bare", "svd"), SAME_REGISTER)
def test_the_bare_and_the_svd_name_reach_one_register(
    board: popcorn.Machine, peripheral: str, bare: str, svd: str
) -> None:
    view = board.periph[peripheral]
    by_bare, by_svd = view[bare], view[svd]
    assert by_bare.name == by_svd.name == bare
    assert repr(by_svd) == repr(by_bare)
    by_svd.value = 0x5
    assert by_bare.value == 0x5
    by_bare.value = 0xA
    assert by_svd.value == 0xA


def test_registers_and_dir_list_bare_names_only(board: popcorn.Machine) -> None:
    timer = board.periph.TIM1
    assert "CR1" in timer.registers
    assert "TIM1_CR1" not in timer.registers
    assert "CR1" in dir(timer)
    assert "TIM1_CR1" not in dir(timer)
    assert "CR1" in board.periph.TIM8.registers


def test_sub_unit_prefixes_stay(board: popcorn.Machine) -> None:
    assert board.periph.COMP.registers == ("COMP1_CSR", "COMP2_CSR")
    with pytest.raises(AttributeError):
        _ = board.periph.COMP.CSR


def test_the_description_keeps_the_svd_name_as_an_alias(u575: popcorn.PartDescription) -> None:
    rcc = next(p for p in u575.peripherals if p.name == "RCC")
    cr = next(r for r in rcc.registers if r.name == "CR")
    assert cr.aliases == ["RCC_CR"]
    usart = next(p for p in u575.peripherals if p.name == "USART1")
    assert all(r.aliases is None for r in usart.registers)


@pytest.mark.parametrize(("peripheral", "bare", "svd"), SAME_REGISTER)
def test_peripheral_desc_register_finds_a_bare_name_and_an_svd_alias(
    u575: popcorn.PartDescription, peripheral: str, bare: str, svd: str
) -> None:
    unit = next(p for p in u575.peripherals if p.name == peripheral)
    by_bare, by_svd = unit.register(bare), unit.register(svd)
    assert by_bare.name == by_svd.name == bare
    assert (by_bare.offset, by_bare.aliases) == (by_svd.offset, by_svd.aliases)
    assert svd in (by_svd.aliases or [])


def test_peripheral_desc_register_raises_key_error_naming_an_unknown_register(
    u575: popcorn.PartDescription,
) -> None:
    rcc = next(p for p in u575.peripherals if p.name == "RCC")
    usart = next(p for p in u575.peripherals if p.name == "USART1")
    assert usart.register("CR2").name == "CR2"
    with pytest.raises(KeyError, match="RCC has no register TIM1_CR1"):
        rcc.register("TIM1_CR1")
    with pytest.raises(KeyError, match="USART1 has no register USART1_CR2"):
        usart.register("USART1_CR2")


def test_refs_read_one_value_through_both_names_while_running(board: popcorn.Machine) -> None:
    start = board.time

    async def scenario() -> tuple[int, int]:
        task = asyncio.create_task(board.run(for_=us(100)))
        bare, svd = await board.read(
            board.ref.periph.RCC.CR, board.ref.periph.RCC.RCC_CR, at=start + us(10)
        )
        await task
        return bare, svd

    bare, svd = asyncio.run(scenario())
    assert bare == svd == board.periph.RCC.CR.value
    assert repr(board.ref.periph.RCC.RCC_CR) == repr(board.ref.periph.RCC.CR)
    assert "periph.RCC.CR" in repr(board.ref.periph.RCC.RCC_CR)


def test_a_trace_names_the_bare_register(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    assert asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000)).reason is (
        popcorn.StopReason.ADDRESS
    )
    names = {r.register for r in m.ring.records() if isinstance(r, Mmio) and r.register}
    assert names & {"RCC.AHB2ENR1", "GPIOA.MODER", "GPIOB.MODER", "GPIOC.MODER"}
    assert not {name for name in names if name.startswith(("RCC.RCC_", "GPIOA.GPIO_"))}

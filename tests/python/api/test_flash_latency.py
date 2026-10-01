"""The FLASH wait-state check of the built-in STM32 models, raised from an event of a run."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, StopReason
from popcorn.units import ms

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
FLASH_ELF = PREBUILT / "flash_ll.elf"
VOS_RANGE1 = 0b11 << 16
HSION = 1 << 8
SW_HSI16 = 0b01


def register(description: popcorn.MachineDescription, peripheral: str, name: str) -> int:
    unit = next(p for p in description.peripherals if p.name == peripheral)
    return unit.base + next(r.offset for r in unit.registers if r.name == name)


def test_lowering_the_range_under_16_mhz_raises_model_error_from_the_run(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(FLASH_ELF), strict_peripherals=True)
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    vosr = register(u575, "PWR", "PWR_VOSR")
    m.mem.u32[vosr] = VOS_RANGE1
    asyncio.run(m.run(for_=ms(1)))
    assert m.periph.PWR.PWR_SVMSR.ACTVOS == 0b11
    m.mem.u32[register(u575, "RCC", "RCC_CR")] |= HSION
    asyncio.run(m.run(for_=ms(1)))
    m.mem.u32[register(u575, "RCC", "RCC_CFGR1")] = SW_HSI16
    asyncio.run(m.run(for_=ms(1)))
    assert m.periph.RCC.RCC_CFGR1.SWS == SW_HSI16
    assert m.periph.FLASH.FLASH_ACR.LATENCY == 0

    m.mem.u32[vosr] = 0
    with pytest.raises(ModelError, match="FLASH"):
        asyncio.run(m.run(for_=ms(1)))

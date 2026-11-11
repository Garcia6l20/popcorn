"""Bits no field of a register covers ignore writes and keep their reset value."""

from __future__ import annotations

import popcorn
import pytest
from popcorn.stm32 import mcu

COVERED = 0x0101_FCFC


@pytest.mark.parametrize("part", ["STM32U575QGIx", "STM32H573AIIx"])
def test_the_reserved_bits_of_tim1_ccmr3_ignore_writes(part: str) -> None:
    m = popcorn.Machine(mcu(part))
    m.periph.TIM1.CCMR3.write(0xFFFF_FFFF)
    assert m.periph.TIM1.CCMR3.read() == COVERED


@pytest.mark.parametrize("part", ["STM32U575QGIx", "STM32H573AIIx"])
def test_the_dbgmcu_idcode_keeps_the_bits_outside_its_fields(part: str) -> None:
    """Oracle check, no mutation reaches it: the DBGMCU model claims IDCODE, so the register layer
    never holds the read."""
    description = mcu(part)
    unit = next(p for p in description.peripherals if p.name == "DBGMCU")
    idcode = next(r for r in unit.registers if r.name == "IDCODE")
    m = popcorn.Machine(description)
    assert m.periph.DBGMCU.IDCODE.read() == idcode.reset & idcode.reset_mask
    covered = 0
    for field in idcode.fields:
        covered |= ((1 << field.width) - 1) << field.lsb
    assert idcode.reset & ~covered != 0


def test_the_u5_adc12_ccr_holds_dual_delay_and_damdf() -> None:
    m = popcorn.Machine(mcu("STM32U575QGIx"))
    ccr = m.periph.ADC12.ADC12_CCR if hasattr(m.periph.ADC12, "ADC12_CCR") else m.periph.ADC12.CCR
    ccr.write(0xFFFF_FFFF)
    assert ccr.read() == 0x01FC_CF1F
    assert ccr.DUAL == 0x1F
    assert ccr.DELAY == 0xF
    assert ccr.DAMDF == 0x3

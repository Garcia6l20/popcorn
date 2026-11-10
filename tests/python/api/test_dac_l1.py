"""The STM32L1 DAC: a holding register write loads the output register one APB1 cycle later."""

from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, support
from popcorn.units import us

PREBUILT = (
    Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "l152xe" / "l1_rcc_ll.elf"
)
L1_PARTS = ("STM32L100RCTx", "STM32L151RETx", "STM32L152RETx", "STM32L162RETx")


@pytest.mark.parametrize("part", L1_PARTS)
def test_every_l1_category_has_a_dac_model(part: str) -> None:
    assert {p.name: p.model for p in support(mcu(part))}["DAC"] == "builtin"


async def test_a_holding_register_write_loads_the_output_register() -> None:
    elf = popcorn.Elf(PREBUILT)
    m = popcorn.MachineDescription(part="STM32L152RETx", images=[elf]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    m.periph.RCC.APB1ENR.DACEN = 1
    m.periph.DAC.DHR12R1.write(0x123)
    assert m.periph.DAC.DOR1.read() == 0
    await m.run(for_=us(10))
    assert m.periph.DAC.DOR1.read() == 0x123
    m.periph.DAC.CR.write(1 << 2 | 7 << 3)
    m.periph.DAC.DHR12R1.write(0x456)
    m.periph.DAC.SWTRIGR.write(1)
    await m.run(for_=us(10))
    assert m.periph.DAC.DOR1.read() == 0x456

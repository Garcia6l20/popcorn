"""The STM32U5 and STM32H5 DAC: DAC1RDY after ENx and the signed input format."""

from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu, support
from popcorn.units import us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
PARTS = (
    ("STM32U575QGIx", "DAC1", PREBUILT / "rcc_ll.elf", "AHB3ENR", "DAC1EN"),
    ("STM32H573AIIx", "DAC", PREBUILT / "h573" / "rcc_ll.elf", "AHB2ENR", "DAC12EN"),
)


@pytest.mark.parametrize(("part", "instance", "elf", "reg", "field"), PARTS)
def test_the_dac_has_a_builtin_model(
    part: str, instance: str, elf: Path, reg: str, field: str
) -> None:
    assert {p.name: p.model for p in support(mcu(part))}[instance] == "builtin"


@pytest.mark.parametrize(("part", "instance", "elf", "reg", "field"), PARTS)
async def test_dac1rdy_sets_after_en1_and_the_signed_format_inverts_the_msb(
    part: str, instance: str, elf: Path, reg: str, field: str
) -> None:
    m = popcorn.MachineDescription(part=part, images=[popcorn.Elf(elf)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    rcc = m.periph.RCC
    setattr(getattr(rcc, reg), field, 1)
    dac = getattr(m.periph, instance)
    dac.CR.write(1)
    assert dac.SR.DAC1RDY == 0
    await m.run(for_=us(10))
    assert dac.SR.DAC1RDY == 1
    dac.MCR.write(1 << 9)
    dac.DHR12R1.write(0x000)
    await m.run(for_=us(1))
    assert dac.DOR1.read() & 0xFFF == 0x800

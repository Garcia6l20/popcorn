"""OTG_FS GRSTCTL.CSRST is writable on the STM32U575 and STM32U585."""

import pytest
from popcorn.stm32 import mcu


@pytest.mark.timeout(120)
@pytest.mark.parametrize("part", ["STM32U575QGIx", "STM32U585QIIx"])
@pytest.mark.parametrize("peripheral", ["OTG_FS", "SEC_OTG_FS"])
def test_the_core_soft_reset_bit_is_read_write(part: str, peripheral: str) -> None:
    otg = next(p for p in mcu(part).peripherals if p.name == peripheral)
    reset = next(r for r in otg.registers if r.name == "GRSTCTL")
    csrst = next(f for f in reset.fields if f.name == "CSRST")
    assert (csrst.lsb, csrst.width, str(csrst.acc)) == (0, 1, "Access.read_write")

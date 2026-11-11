"""The backup domain words of the U5 and H5 PWR read through the register views."""

from __future__ import annotations

import popcorn
import pytest
from popcorn.stm32 import mcu


@pytest.mark.parametrize(
    ("part", "registers"),
    [("STM32U575QGIx", ("BDCR1", "BDCR2", "BDSR")), ("STM32H573AIIx", ("BDCR", "BDSR"))],
)
def test_a_strict_machine_reads_the_backup_domain_words(
    part: str, registers: tuple[str, ...]
) -> None:
    m = popcorn.Machine(mcu(part), strict_peripherals=True)
    for name in registers:
        assert getattr(m.periph.PWR, name).read() == 0

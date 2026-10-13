"""MachineDescription of the adc_watchdog example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import SupplyOptions

HERE = Path(__file__).parent

ADC_WATCHDOG = popcorn.MachineDescription(
    name="adc-watchdog",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/adc_watchdog.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    supply=SupplyOptions(vdda_mv=3300),
    strict=True,
)


@popcorn.pytest.machine
def adc_watchdog() -> popcorn.MachineDescription:
    return ADC_WATCHDOG

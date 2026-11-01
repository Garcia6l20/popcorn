"""MachineDescriptions of the adc example, at the default 3.0 V VDDA and at 3.3 V."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import SupplyOptions

HERE = Path(__file__).parent

BASE = popcorn.MachineDescription(
    name="adc",
    part="STM32L152RETx",
    images=[popcorn.Elf(HERE / "prebuilt/adc_internal.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    strict=True,
)

ADC = BASE.with_configs(vdda_3v3=BASE.replace(supply=SupplyOptions(vdda_mv=3300)))


@popcorn.pytest.machine
def adc() -> popcorn.MachineDescription:
    return ADC


@popcorn.pytest.machine(config="vdda_3v3")
def adc_3v3() -> popcorn.MachineDescription:
    return ADC

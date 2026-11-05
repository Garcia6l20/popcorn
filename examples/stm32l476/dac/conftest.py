"""MachineDescriptions of the dac example on the STM32L476RCTx, at the default 3.0 V VDDA and at
3.3 V."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import SupplyOptions

HERE = Path(__file__).parent

BASE = popcorn.MachineDescription(
    name="dac",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/dac_triangle.elf")],
    boot=popcorn.Boot(until="wave_started", budget=1_000_000),
    strict=True,
)

DAC = BASE.with_configs(vdda_3v3=BASE.replace(supply=SupplyOptions(vdda_mv=3300)))


@popcorn.pytest.machine
def dac() -> popcorn.MachineDescription:
    return DAC


@popcorn.pytest.machine(config="vdda_3v3")
def dac_3v3() -> popcorn.MachineDescription:
    return DAC

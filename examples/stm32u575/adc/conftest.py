"""MachineDescription of the adc example."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="adc-internal",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/adc_internal.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)


@popcorn.pytest.machine
def adc() -> popcorn.MachineDescription:
    return PROJECT

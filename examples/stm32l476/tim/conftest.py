"""MachineDescription of the tim example on the STM32L476RCTx."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent

TIM = popcorn.MachineDescription(
    name="tim",
    part="STM32L476RCTx",
    images=[popcorn.Elf(HERE / "prebuilt/tim_ticks.elf")],
    boot=popcorn.Boot(until="timers_started", budget=1_000_000),
    strict=True,
)


@popcorn.pytest.machine
def tim() -> popcorn.MachineDescription:
    return TIM

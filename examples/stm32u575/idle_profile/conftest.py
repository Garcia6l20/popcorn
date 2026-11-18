"""MachineDescriptions of the idle_profile example, one per firmware variant."""

from pathlib import Path

import popcorn
import popcorn.pytest

HERE = Path(__file__).parent


def description(name: str) -> popcorn.MachineDescription:
    """The STM32U575 running prebuilt/@p name.elf, stopped at `main`."""
    return popcorn.MachineDescription(
        name=name.replace("_", "-"),
        part="STM32U575QGIx",
        images=[popcorn.Elf(HERE / f"prebuilt/{name}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
    )


IDLE_WFI = description("idle_wfi")
IDLE_SPIN = description("idle_spin")


@popcorn.pytest.machine
def idle_wfi() -> popcorn.MachineDescription:
    return IDLE_WFI


@popcorn.pytest.machine
def idle_spin() -> popcorn.MachineDescription:
    return IDLE_SPIN

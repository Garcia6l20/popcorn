"""MachineDescriptions of the rcc example, the xe build on the STM32L152RETx and the xb build on
the Cat.1 STM32L152RBTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.board import RccOptions

HERE = Path(__file__).parent

RCC = {
    image: popcorn.MachineDescription(
        name=f"rcc-clock-{image}",
        part=part,
        images=[popcorn.Elf(HERE / f"prebuilt/{elf}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
        strict=True,
        rcc=RccOptions(hse_hz=8_000_000),
    )
    for image, part, elf in (
        ("xe", "STM32L152RETx", "rcc_clock"),
        ("xb", "STM32L152RBTx", "rcc_clock_xb"),
    )
}


@popcorn.pytest.machine(params=list(RCC))
def rcc(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return RCC[request.param]

"""MachineDescriptions of the pwr_flash example, the xe build on the STM32L152RETx and the xb
build on the Cat.1 STM32L152RBTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

PWR_FLASH = {
    image: popcorn.MachineDescription(
        name=f"pwr-flash-{image}",
        part=part,
        images=[popcorn.Elf(HERE / f"prebuilt/{elf}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
        strict=True,
    )
    for image, part, elf in (
        ("xe", "STM32L152RETx", "pwr_flash"),
        ("xb", "STM32L152RBTx", "pwr_flash_xb"),
    )
}


@popcorn.pytest.machine(params=list(PWR_FLASH))
def pwr_flash(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return PWR_FLASH[request.param]

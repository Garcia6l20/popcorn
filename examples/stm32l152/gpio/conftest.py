"""MachineDescriptions of the gpio example, the xe build on the STM32L152RETx and the xb build
on the Cat.1 STM32L152RBTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

GPIO = {
    image: popcorn.MachineDescription(
        name=f"gpio-button-{image}",
        part=part,
        images=[popcorn.Elf(HERE / f"prebuilt/{elf}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
    )
    for image, part, elf in (
        ("xe", "STM32L152RETx", "gpio_button"),
        ("xb", "STM32L152RBTx", "gpio_button_xb"),
    )
}


@popcorn.pytest.machine(params=list(GPIO))
def gpio(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return GPIO[request.param]


@pytest.fixture(scope="session", params=list(GPIO))
def gpio_description(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return GPIO[request.param]

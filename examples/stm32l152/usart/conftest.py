"""MachineDescriptions of the usart example, the xe build on the STM32L152RETx and the xb build
on the Cat.1 STM32L152RBTx."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

USART = {
    image: popcorn.MachineDescription(
        name=f"usart-echo-{image}",
        part=part,
        images=[popcorn.Elf(HERE / f"prebuilt/{elf}.elf")],
        boot=popcorn.Boot(until="main", budget=1_000_000),
    )
    for image, part, elf in (
        ("xe", "STM32L152RETx", "usart_echo"),
        ("xb", "STM32L152RBTx", "usart_echo_xb"),
    )
}


@popcorn.pytest.machine(params=list(USART))
def usart(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return USART[request.param]


@pytest.fixture(scope="session", params=list(USART))
def usart_description(request: pytest.FixtureRequest) -> popcorn.MachineDescription:
    return USART[request.param]

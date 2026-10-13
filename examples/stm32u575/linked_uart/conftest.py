"""The linked_uart example: one controller and two responders, alone or linked.

`controller`, `responder1` and `responder2` are machines, the two responders built from one
description and restored from one boot. `system` links the three as the firmware expects.
`system_description` is the linked system's description, for the tests that read or vary it."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.units import us

HERE = Path(__file__).parent

CONTROLLER = popcorn.MachineDescription(
    name="linked-controller",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/linked_controller.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)

RESPONDER = popcorn.MachineDescription(
    name="linked-responder",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/linked_responder.elf")],
    boot=popcorn.Boot(until="links_ready", budget=2_000_000),
)

LATENCY = us(20)

SYSTEM = popcorn.SystemDescription(
    members={"controller": CONTROLLER, "responder1": RESPONDER, "responder2": RESPONDER},
    links=[
        popcorn.UartLink("controller.UART4", "responder1.USART3", latency=LATENCY, baud=460_800),
        popcorn.UartLink("controller.USART3", "responder2.USART3", latency=LATENCY, baud=460_800),
    ],
)


@popcorn.pytest.machine
def controller() -> popcorn.MachineDescription:
    return CONTROLLER


@popcorn.pytest.machine
def responder1() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.machine
def responder2() -> popcorn.MachineDescription:
    return RESPONDER


@popcorn.pytest.system
def system() -> popcorn.SystemDescription:
    return SYSTEM


@pytest.fixture(scope="session")
def system_description() -> popcorn.SystemDescription:
    return SYSTEM

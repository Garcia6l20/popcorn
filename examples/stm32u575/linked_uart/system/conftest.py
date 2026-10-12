"""The linked system of the linked_uart example: one controller, two responders."""

import popcorn
import pytest
from linked_projects import CONTROLLER, RESPONDER
from popcorn.units import us

LATENCY = us(20)

SYSTEM = popcorn.System(
    members={"controller": CONTROLLER, "responder1": RESPONDER, "responder2": RESPONDER},
    links=[
        popcorn.UartLink("controller.UART4", "responder1.USART3", latency=LATENCY, baud=460_800),
        popcorn.UartLink("controller.USART3", "responder2.USART3", latency=LATENCY, baud=460_800),
    ],
)


@pytest.fixture(scope="session")
def popcorn_system() -> popcorn.System:
    return SYSTEM

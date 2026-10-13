"""The controller on its own, nothing on its links: it offers a node id to each responder at
start and again every 1.5 s, since no answer comes."""

from popcorn import Machine, StopReason
from popcorn.units import ms


async def test_the_controller_offers_node_ids_until_answered(controller: Machine) -> None:
    stop = await controller.run(for_=ms(1600))
    assert stop.reason is StopReason.TIME, stop
    assert controller.periph.UART4.port.take() == b"N\x01" * 2
    assert controller.periph.USART3.port.take() == b"N\x02" * 2

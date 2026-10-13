"""The responder on its own, the test playing the controller on its USART3."""

from popcorn import Machine, StopReason
from popcorn.units import ms

BLOCK_BYTES = 512


def block(seq: int) -> bytes:
    """The controller's block `seq`: a header, then a counter from `seq`."""
    return bytes([ord("B"), seq, *((seq + index) & 0xFF for index in range(2, BLOCK_BYTES))])


def checksum(data: bytes) -> int:
    """The firmware's `block_checksum`: the byte sum low, the sum of sums high, each 16 bits."""
    total = sums = 0
    for byte in data:
        total = (total + byte) & 0xFFFF
        sums = (sums + total) & 0xFFFF
    return sums << 16 | total


async def test_a_responder_answers_only_once_it_has_a_node_id(responder1: Machine) -> None:
    link = responder1.periph.USART3.port
    link.write(b"P\x05\x00\x00")
    await responder1.run(for_=ms(5))
    assert link.take() == b""

    link.write(b"N\x07")
    await responder1.run(for_=ms(5))
    assert link.take() == b"n\x07"
    assert responder1.var("node_id").value == 7

    link.write(block(3))
    stop = await responder1.run(for_=ms(30))
    assert stop.reason is StopReason.TIME, stop
    answer = link.take()
    assert len(answer) == BLOCK_BYTES
    assert answer[:3] == b"b\x07\x03"
    assert int.from_bytes(answer[3:7], "little") == checksum(block(3))
    assert answer[7:] == bytes([7]) * (BLOCK_BYTES - 7)
    assert b"responder: node id 7\r\n" in responder1.periph.USART1.port.take()

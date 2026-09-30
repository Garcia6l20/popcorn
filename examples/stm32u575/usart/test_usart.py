"""A printf banner and a line echo on USART1, 115200 8N1, HAL interrupt-mode receive.

`machine.periph.USART1.port` is the far end of the USART: `read_frames` returns what the
firmware sent with the virtual time each frame ended, `write` queues bytes for its RX line.

USART1 runs from PCLK2 at the 4 MHz reset clock, so BRR is 35 and a 10-bit frame lasts
87.5 us, 114286 baud. The frame check allows 1 ns: the model derives an integer baud from BRR
first, which adds 546 ps per frame.
"""

import itertools

import pytest
from popcorn import Machine
from popcorn.units import ms, ns

pytestmark = pytest.mark.popcorn(budget=2_000_000)

BANNER = b"popcorn usart example, 115200 baud\r\n"
PCLK2_HZ = 4_000_000


async def test_the_banner_comes_out_at_the_brr_rate(machine: Machine) -> None:
    frames = await machine.periph.USART1.port.read_frames(len(BANNER), for_=ms(10))
    assert bytes(frame.data for frame in frames) == BANNER

    brr = machine.periph.USART1.BRR.value
    assert brr == round(PCLK2_HZ / 115_200)
    frame_ps = 10 * brr * 10**12 // PCLK2_HZ
    for earlier, later in itertools.pairwise(frames):
        assert abs((later.at - earlier.at).ps - frame_ps) < ns(1).ps


async def test_a_line_comes_back_prefixed(machine: Machine) -> None:
    port = machine.periph.USART1.port
    await port.read(len(BANNER), for_=ms(10))
    await port.write(b"hello\r\n")
    assert await port.read(13, for_=ms(10)) == b"echo: hello\r\n"


async def test_each_line_is_echoed_in_turn(machine: Machine) -> None:
    port = machine.periph.USART1.port
    await port.read(len(BANNER), for_=ms(10))
    for word in (b"one", b"two", b"three"):
        await port.write(word + b"\r\n")
        expected = b"echo: " + word + b"\r\n"
        assert await port.read(len(expected), for_=ms(10)) == expected

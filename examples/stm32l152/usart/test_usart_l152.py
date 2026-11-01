"""A printf banner and a line echo on USART2, 9600 8N1, PA2 and PA3 on AF7, HAL interrupt-mode
receive, on the xe and the xb build.

`machine.periph.USART2.port` is the far end of the UART: `read_frames` returns what the firmware
sent with the virtual time each frame ended, `write` queues bytes for its RX line.

USART2 runs from PCLK1 at the MSI reset clock. The HAL computes BRR from MSI_VALUE, 2.097 MHz,
while the MSI range 5 of the model runs at 65536 << 5, 2.097152 MHz, as RM0038 gives it. BRR is
218, USARTDIV 13.625, so the rate is 9620.0 baud. A frame lasts 10 times BRR kernel clocks,
10 * 218 / 2097152 s, about 1.0395 ms, rounded down to the picosecond, and the frames are exactly
that far apart.
"""

import itertools

import pytest
from popcorn import Machine, MachineDescription, StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=4_000_000)

BANNER = b"popcorn usart example, 9600 baud\r\n"
PCLK1_HZ = 65536 << 5
BRR = 218
AF_MODE = 2
READ = ms(100)


async def test_the_banner_comes_out_at_the_brr_rate(usart: Machine) -> None:
    frames = await usart.periph.USART2.port.read_frames(len(BANNER), for_=READ)
    assert bytes(frame.data for frame in frames) == BANNER

    assert usart.periph.USART2.BRR.value == BRR
    frame_ps = 10 * BRR * 10**12 // PCLK1_HZ
    for earlier, later in itertools.pairwise(frames):
        assert (later.at - earlier.at).ps == frame_ps


async def test_the_pins_are_pa2_and_pa3_on_af7(usart: Machine) -> None:
    await usart.periph.USART2.port.read(len(BANNER), for_=READ)
    gpioa = usart.periph.GPIOA
    assert (gpioa.MODER.MODER2, gpioa.MODER.MODER3) == (AF_MODE, AF_MODE)
    assert (gpioa.AFRL.AFRL2, gpioa.AFRL.AFRL3) == (7, 7)


async def test_a_line_comes_back_prefixed(usart: Machine) -> None:
    port = usart.periph.USART2.port
    await port.read(len(BANNER), for_=READ)
    await port.write(b"hello\r\n")
    assert await port.read(13, for_=READ) == b"echo: hello\r\n"


async def test_each_line_is_echoed_in_turn(usart: Machine) -> None:
    port = usart.periph.USART2.port
    await port.read(len(BANNER), for_=READ)
    for word in (b"one", b"two", b"three"):
        await port.write(word + b"\r\n")
        expected = b"echo: " + word + b"\r\n"
        assert await port.read(len(expected), for_=READ) == expected


async def test_the_banner_is_kept_for_a_port_taken_after_the_run(
    machine_factory: MachineFactory, usart_description: MachineDescription
) -> None:
    fresh = await machine_factory.booted(usart_description)
    stop = await fresh.run(for_=READ)
    assert stop.reason is StopReason.TIME, stop
    assert await fresh.periph.USART2.port.read(len(BANNER), for_=ms(1)) == BANNER

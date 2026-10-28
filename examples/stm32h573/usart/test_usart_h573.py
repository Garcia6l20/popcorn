"""A printf banner and a line echo on UART9, 921600 8N1, PG0 and PG1, HAL interrupt-mode receive.

`machine.periph.UART9.port` is the far end of the UART: `read_frames` returns what the firmware
sent with the virtual time each frame ended, `write` queues bytes for its RX line.

UART9 runs from PCLK1 at the 32 MHz reset clock, HSI divided by 2, so BRR is 35 and a 10-bit
frame lasts 10.9375 us, 914286 baud. The frame check allows 1 ns.
"""

import itertools

import pytest
from popcorn import Machine, MachineDescription, StopReason
from popcorn.pytest import MachineFactory
from popcorn.units import ms, ns

pytestmark = pytest.mark.popcorn(budget=2_000_000)

BANNER = b"popcorn usart example, 921600 baud\r\n"
PCLK1_HZ = 32_000_000
AF_MODE = 2


async def test_the_banner_comes_out_at_the_brr_rate(usart: Machine) -> None:
    frames = await usart.periph.UART9.port.read_frames(len(BANNER), for_=ms(10))
    assert bytes(frame.data for frame in frames) == BANNER

    brr = usart.periph.UART9.BRR.value
    assert brr == round(PCLK1_HZ / 921_600)
    frame_ps = 10 * brr * 10**12 // PCLK1_HZ
    for earlier, later in itertools.pairwise(frames):
        assert abs((later.at - earlier.at).ps - frame_ps) < ns(1).ps


async def test_the_pins_are_pg0_and_pg1_on_af11(usart: Machine) -> None:
    await usart.periph.UART9.port.read(len(BANNER), for_=ms(10))
    gpiog = usart.periph.GPIOG
    assert (gpiog.MODER.MODE0, gpiog.MODER.MODE1) == (AF_MODE, AF_MODE)
    assert (gpiog.AFRL.AFSEL0, gpiog.AFRL.AFSEL1) == (11, 11)
    gpiog.pin(0).level = True
    assert gpiog.IDR.ID0 == 1


async def test_a_line_comes_back_prefixed(usart: Machine) -> None:
    port = usart.periph.UART9.port
    await port.read(len(BANNER), for_=ms(10))
    await port.write(b"hello\r\n")
    assert await port.read(13, for_=ms(10)) == b"echo: hello\r\n"


async def test_each_line_is_echoed_in_turn(usart: Machine) -> None:
    port = usart.periph.UART9.port
    await port.read(len(BANNER), for_=ms(10))
    for word in (b"one", b"two", b"three"):
        await port.write(word + b"\r\n")
        expected = b"echo: " + word + b"\r\n"
        assert await port.read(len(expected), for_=ms(10)) == expected


async def test_the_banner_is_kept_for_a_port_taken_after_the_run(
    machine_factory: MachineFactory, usart_description: MachineDescription
) -> None:
    fresh = await machine_factory.booted(usart_description)
    stop = await fresh.run(for_=ms(10))
    assert stop.reason is StopReason.TIME, stop
    assert await fresh.periph.UART9.port.read(len(BANNER), for_=ms(1)) == BANNER

"""A sine table played on SAI1 block A, I2S 16-bit stereo at 48 kHz, by circular GPDMA, HAL.

`machine.periph.SAI1.audio("A")` is the far end of the block, a codec in place: it holds every
frame the block sent since power-on, with the virtual time the frame started. The table holds
one 1 kHz period, 48 stereo frames, the right channel the negated left one. Slot words are the
16 data bits, right-aligned, so a negative sample reads as its two's complement.

SAI1 runs from PLL2P, MSIS 4 MHz x PLL2N / (PLL2M x PLL2P). With no master clock divider a
frame lasts MCKDIV x (FRL + 1) kernel cycles, RM0456 section 69.4.8. The test derives the frame
time from those registers, as the firmware set them: 15.36 MHz and 10 x 32 cycles, 1/48 kHz
= 20 833 333.3 ps. The model rounds each frame start to the picosecond, so two frames are
20 833 333 or 20 833 334 ps apart.

The DMA moves 96 half-words per table: a half-transfer callback after 48, a complete callback
after 96. A frame counts as sent when it ends, and the frame being shifted has already taken
its 2 words, so the DMA runs up to the 8-word FIFO plus 2 words ahead of the frames sent. In
10 ms the block sends 480 frames, 10 tables, less the setup time between `main` and the first
frame, 0.9 ms, so at least 9 complete callbacks.
"""

import itertools

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=2_000_000)

PERIOD = 48
FRAMES = 10 * PERIOD
FIFO_WORDS = 8
MSIS_HZ = 4_000_000


def frame_ps(machine: Machine) -> float:
    rcc, sai = machine.periph.RCC, machine.periph.SAI1
    kernel_hz = (
        MSIS_HZ
        * (rcc.RCC_PLL2DIVR.PLL2N + 1)
        / ((rcc.RCC_PLL2CFGR.PLL2M + 1) * (rcc.RCC_PLL2DIVR.PLL2P + 1))
    )
    cycles = sai.ACR1.MCKDIV * (sai.AFRCR.FRL + 1)
    return cycles * 10**12 / kernel_hz


def table(machine: Machine) -> list[tuple[int, int]]:
    sine = machine.var("sine")
    words = [int(sine[index].value) & 0xFFFF for index in range(2 * PERIOD)]
    return list(itertools.batched(words, 2, strict=True))


async def test_the_frames_are_the_table_repeated(machine: Machine) -> None:
    stream = machine.periph.SAI1.audio("A")
    frames = await stream.read_frames(FRAMES, for_=ms(20))
    assert [frame.slots for frame in frames] == table(machine) * 10
    assert all(
        (frame.data_bits, frame.active_slots, frame.mono) == (16, 0b11, False) for frame in frames
    )
    assert stream.dropped == 0


async def test_frames_come_at_48_khz(machine: Machine) -> None:
    frames = await machine.periph.SAI1.audio("A").read_frames(FRAMES, for_=ms(20))
    period = frame_ps(machine)
    assert abs(period - 10**12 / 48_000) < 1
    for earlier, later in itertools.pairwise(frames):
        assert abs((later.at - earlier.at).ps - period) < 1
    assert abs((frames[-1].at - frames[0].at).ps - (FRAMES - 1) * period) < 1


async def test_the_dma_callbacks_follow_the_frames_sent(machine: Machine) -> None:
    stop = await machine.run(for_=ms(10))
    assert stop.reason is StopReason.TIME, stop
    sent_words = 2 * machine.periph.SAI1.audio("A").available
    playback = machine.var("playback")
    halves, completes = int(playback.halves.value), int(playback.completes.value)
    assert completes >= 9
    assert halves - completes in (0, 1)
    moved = PERIOD * (halves + completes)
    assert -(FIFO_WORDS + 2) <= sent_words - moved < PERIOD

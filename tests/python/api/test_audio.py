"""SAI audio streams: frames a block transmits, captured from Python in virtual time.

The LL test firmware's SAI1 block A is a 16-bit stereo master whose frame lasts 64 us, the
native SAI test's frame time. A frame's `at` is its start."""

from __future__ import annotations

import asyncio
import itertools
import struct
from pathlib import Path

import popcorn
import pytest
from popcorn import StopReason
from popcorn.units import ms, us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
SAI_ELF = PREBUILT / "sai_ll.elf"
BUDGET = 100_000_000
FRAMES = 64
ACCESSOR = "audio"
FRAME = us(64)


def ramp(index: int) -> int:
    return 0x1000 + index


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    raw = m.mem.read(m.sym("log_buf"), 4 * min(count, 256))
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def run_entry(m: popcorn.Machine, entry: str) -> list[int]:
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS, stop
    m.reg.pc = m.sym(entry)
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER, stop
    return log(m)


@pytest.mark.xfail(
    strict=True, raises=AttributeError, reason="SAI: no Python capture of an audio block"
)
def test_frames_a_block_sent_before_the_stream_was_taken_are_captured(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SAI_ELF))
    assert run_entry(m, "tx_poll") == [0]

    stream = getattr(m.periph.SAI1, ACCESSOR)("A")
    assert stream.available == FRAMES
    frames = asyncio.run(stream.read_frames(FRAMES, for_=ms(1)))
    assert [frame.slots for frame in frames] == [
        (ramp(2 * index), ramp(2 * index + 1)) for index in range(FRAMES)
    ]
    assert all(later.at - earlier.at == FRAME for earlier, later in itertools.pairwise(frames))
    assert all(
        (frame.data_bits, frame.active_slots, frame.mono) == (16, 0b11, False) for frame in frames
    )
    assert (stream.available, stream.dropped) == (0, 0)

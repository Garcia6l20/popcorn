"""A machine poke of a layout register on a running SAI block changes the slot map of its next
frame."""

from __future__ import annotations

import asyncio

import popcorn
from popcorn import StopReason

from .test_audio import BUDGET, FRAME, sai_machine


def test_a_poked_sloten_drops_the_slot_from_the_next_frames(u575: popcorn.PartDescription) -> None:
    m = sai_machine(u575)
    stream = m.periph.SAI1.audio("A")
    m.reg.pc = m.sym("tx_poll")
    asyncio.run(m.run(for_=FRAME * 8, budget=BUDGET))
    m.periph.SAI1.ASLOTR.poke(SLOTEN=2)
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER, stop
    frames = asyncio.run(stream.read_frames(stream.available))
    assert len(frames) > 16
    assert all(frame.slots[0] != 0 and frame.slots[1] != 0 for frame in frames[:6])
    assert all(frame.slots[0] == 0 and frame.slots[1] != 0 for frame in frames[-6:])

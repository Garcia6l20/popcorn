"""Every CCMR1 and CCMR2 word of every shipped timer keeps the bits of each of its views.

The CCMR1 and CCMR2 words have an input and an output view at one offset, and some SVDs give a
bit in one view only, such as IC1F bit 3 on the H573 TIM13. The oracle is the description: the
union of the read-write fields over every view at the word. The STM32L4 timers are storage, so
their parts are left out.
"""

from __future__ import annotations

import popcorn
import pytest
from popcorn._core import Access, PeripheralDesc
from popcorn.stm32 import shipped_parts

PARTS = [part for part in shipped_parts() if not part.startswith("STM32L4")]


def ccmr_words(peripheral: PeripheralDesc) -> dict[int, int]:
    words: dict[int, int] = {}
    for register in peripheral.registers:
        if not register.name.removeprefix("TIM1_").startswith(("CCMR1", "CCMR2")):
            continue
        mask = words.setdefault(register.offset, 0)
        for field in register.fields:
            if field.acc == Access.read_write:
                mask |= ((1 << field.width) - 1) << field.lsb
        words[register.offset] = mask
    return words


@pytest.mark.parametrize("part", PARTS)
def test_every_ccmr_word_reads_back_the_union_of_its_views(part: str) -> None:
    machine = popcorn.MachineDescription(part=part, images=[]).machine()
    machine.reset()
    checked = 0
    for peripheral in machine.description.peripherals:
        if not peripheral.name.startswith("TIM"):
            continue
        for offset, mask in ccmr_words(peripheral).items():
            address = peripheral.base + offset
            machine.mem.u32[address] = 0xFFFF_FFFF
            assert machine.mem.u32[address] == mask, (peripheral.name, hex(offset))
            machine.mem.u32[address] = 0
            checked += 1
    assert checked > 0

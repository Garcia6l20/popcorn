"""The fault policy: a processor fault stops the run, or enters the firmware's handler."""

from __future__ import annotations

import asyncio
import struct
from pathlib import Path
from typing import Literal

import popcorn
import pytest
from popcorn import ConfigError, StopReason

ROOT = Path(__file__).parents[3]
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
DIGEST_AT_MAIN = "c1fef57e3020d0713b9ead07c1fab2394e8855dce16a87439f02662500bcd068"

TABLE = 0x2001_0000
FAULTING = 0x2001_0300
HARD_FAULT = 0x2001_0400
USAGE_FAULT = 0x2001_0410
STACK = 0x2001_8000
VTOR = 0xE000_ED08
SHCSR = 0xE000_ED24
HFSR = 0xE000_ED2C
UFSR_UNDEFINSTR = 1 << 16
SHCSR_USGFAULTENA = 1 << 18
HFSR_FORCED = 1 << 30
UDF = 0xDE07
SPIN = 0xE7FE


def faulting_machine(
    u575: popcorn.PartDescription, faults: Literal["stop", "handler"], usage_enabled: bool = False
) -> popcorn.Machine:
    """A UDF in SRAM, DDI0553B.z C2.4.390, with a vector table whose HardFault and UsageFault
    handlers each spin at their own address."""
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), faults=faults)
    m.reset()
    m.mem.u32[TABLE + 4 * 3] = HARD_FAULT | 1
    m.mem.u32[TABLE + 4 * 6] = USAGE_FAULT | 1
    m.mem.u32[VTOR] = TABLE
    if usage_enabled:
        m.mem.u32[SHCSR] |= SHCSR_USGFAULTENA
    m.mem.write(FAULTING, struct.pack("<H", UDF))
    m.mem.write(HARD_FAULT, struct.pack("<H", SPIN))
    m.mem.write(USAGE_FAULT, struct.pack("<H", SPIN))
    m.reg.msp = STACK
    m.reg.pc = FAULTING
    return m


def test_the_default_policy_stops_at_the_faulting_instruction(
    u575: popcorn.PartDescription,
) -> None:
    m = faulting_machine(u575, "stop")
    stop = asyncio.run(m.run(budget=100))
    assert stop.reason is StopReason.FAULT
    assert stop.pc == FAULTING
    assert m.reg.xpsr & 0x1FF == 0


def test_the_handler_policy_escalates_a_disabled_usagefault_to_hardfault(
    u575: popcorn.PartDescription,
) -> None:
    """DDI0553B.z B3.27: a UsageFault with SHCSR.USGFAULTENA 0 escalates to HardFault and sets
    HFSR.FORCED."""
    m = faulting_machine(u575, "handler")
    stop = asyncio.run(m.run(until=HARD_FAULT, budget=100))
    assert stop.reason is StopReason.ADDRESS, stop
    assert m.reg.xpsr & 0x1FF == 3
    assert m.mem.u32[HFSR] & HFSR_FORCED


def test_the_handler_policy_enters_an_enabled_usagefault(u575: popcorn.PartDescription) -> None:
    m = faulting_machine(u575, "handler", usage_enabled=True)
    stop = asyncio.run(m.run(until=USAGE_FAULT, budget=100))
    assert stop.reason is StopReason.ADDRESS, stop
    assert m.reg.xpsr & 0x1FF == 6
    assert m.mem.u32[0xE000_ED28] & UFSR_UNDEFINSTR
    assert not m.mem.u32[HFSR] & HFSR_FORCED


def test_a_description_passes_its_policy_to_the_machine(u575: popcorn.PartDescription) -> None:
    description = popcorn.MachineDescription(
        part="STM32U575QGIx", images=[popcorn.Elf(RCC_ELF)], faults="handler"
    )
    m = description.machine()
    m.reset()
    m.mem.u32[TABLE + 4 * 3] = HARD_FAULT | 1
    m.mem.u32[VTOR] = TABLE
    m.mem.write(FAULTING, struct.pack("<H", UDF))
    m.mem.write(HARD_FAULT, struct.pack("<H", SPIN))
    m.reg.msp = STACK
    m.reg.pc = FAULTING
    stop = asyncio.run(m.run(until=HARD_FAULT, budget=100))
    assert stop.reason is StopReason.ADDRESS, stop
    del u575


def test_the_policy_enters_the_digest_only_when_it_is_not_stop() -> None:
    plain = popcorn.MachineDescription(part="STM32U575QGIx", images=())
    assert plain.digest == DIGEST_AT_MAIN
    assert plain.replace(faults="stop").digest == DIGEST_AT_MAIN
    assert plain.replace(faults="handler").digest != DIGEST_AT_MAIN


def test_an_unknown_policy_is_refused_before_anything_is_built(
    u575: popcorn.PartDescription,
) -> None:
    with pytest.raises(ValueError, match=r'faults is "stop" or "handler", not \'enter\''):
        popcorn.Machine(u575, faults="enter")  # pyright: ignore[reportArgumentType]
    with pytest.raises(ConfigError, match=r'^faults: expected "stop" or "handler"'):
        popcorn.MachineDescription(
            part="STM32U575QGIx",
            images=(),
            faults="enter",  # pyright: ignore[reportArgumentType]
        )

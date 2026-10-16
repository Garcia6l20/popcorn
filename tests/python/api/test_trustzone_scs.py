"""TrustZone system space: the SAU slice stop and snapshots across the TrustZone change."""

from __future__ import annotations

import asyncio
import gzip
import struct
from pathlib import Path

import popcorn
import pytest
from popcorn import OptionBytes, Snapshot, StopReason
from popcorn.units import ms

ROOT = Path(__file__).parents[3]
SCS_ELF = ROOT / "tests" / "firmware" / "arch" / "prebuilt" / "scs.elf"
RCC_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
SAVE_BEFORE_TRUSTZONE = ROOT / "tests" / "data" / "core" / "scs_before_trustzone.bson.gz"

CODE = 0x2008_0000
SAU_CTRL = 0xE000_EDD0
SAU_RNR = 0xE000_EDD8
SAU_RBAR = 0xE000_EDDC
SAU_RLAR = 0xE000_EDE0
SCS_REGISTERS = (
    0xE000_ED08,
    0xE000_ED0C,
    0xE000_ED10,
    0xE000_ED14,
    0xE000_ED18,
    0xE000_ED1C,
    0xE000_ED20,
    0xE000_ED24,
    0xE000_ED88,
    0xE000_ED94,
    0xE000_EF34,
    0xE000_E100,
    0xE000_E400,
)
CORE_REGISTERS = (
    "r0",
    "r1",
    "r2",
    "r3",
    "r4",
    "r12",
    "sp",
    "lr",
    "pc",
    "xpsr",
    "msp",
    "psp",
    "control",
)
SAU_LOOP = (
    0x3301,
    0x42AB,
    0xBF08,
    0x600A,
    0xF3BF,
    0x8F4F,
    0xF3BF,
    0x8F6F,
    0x42BB,
    0xD1F5,
    0xBE02,
)


def scs_machine(m33: popcorn.PartDescription) -> popcorn.Machine:
    return popcorn.Machine(m33, image=popcorn.Image.elf(SCS_ELF))


def state_of(m: popcorn.Machine) -> tuple[tuple[int, ...], tuple[int, ...]]:
    return (
        tuple(int(getattr(m.reg, name)) for name in CORE_REGISTERS),
        tuple(m.mem.u32[at] for at in SCS_REGISTERS),
    )


def test_a_save_from_before_trustzone_restores_as_a_save_taken_today(
    m33: popcorn.PartDescription,
) -> None:
    """P7: the arch state of a format 5 save from before TrustZone decodes with the Secure bank
    only, TZEN=0, and runs on as a save of the same point taken by this build."""
    old = scs_machine(m33)
    old.restore(Snapshot.from_bytes(gzip.decompress(SAVE_BEFORE_TRUSTZONE.read_bytes())))
    new = scs_machine(m33)
    new.reset()
    assert asyncio.run(new.run(budget=2_000_000)).reason is StopReason.USER
    assert state_of(old) == state_of(new)
    stops: list[tuple[StopReason, int]] = []
    for m in (old, new):
        m.reg.pc = m.sym("scs_run")
        stop = asyncio.run(m.run(budget=1_000_000))
        stops.append((stop.reason, stop.pc))
    assert stops[0] == stops[1]
    assert stops[0][0] is StopReason.USER
    assert state_of(old) == state_of(new)


def sau_loop(u575: popcorn.PartDescription, running: bool) -> tuple[int, int, StopReason]:
    """A self-chained Secure loop that stores SAU_RLAR at iteration 10, making its own page
    Non-secure. Returns the stop PC, r3 and the reason, parked or free."""
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        option_bytes=OptionBytes(tzen=True),
        slice_instructions=1_000_000,
        slice_wall_budget=1.0,
    )
    m.reset()
    m.mem.write(CODE, struct.pack(f"<{len(SAU_LOOP)}H", *SAU_LOOP))
    m.mem.u32[SAU_RNR] = 0
    m.mem.u32[SAU_RBAR] = CODE
    m.mem.u32[SAU_RLAR] = CODE + 0x3E0
    m.mem.u32[SAU_CTRL] = 1
    m.reg.r1 = SAU_RLAR
    m.reg.r2 = CODE + 0x3E1
    m.reg.r3 = 0
    m.reg.r5 = 10
    m.reg.r7 = 100_000
    m.reg.pc = CODE

    async def free() -> None:
        with pytest.raises(popcorn.EmulationError, match="stopped with FAULT"):
            async with m.running():
                await m.wait_for(popcorn.Reached(CODE + 0x14), for_=ms(100))

    if running:
        asyncio.run(free())
        reason = StopReason.FAULT
    else:
        reason = asyncio.run(m.run(budget=1_000_000)).reason
    return m.pc, m.reg.r3, reason


@pytest.fixture(scope="module")
def parked_sau_loop(u575: popcorn.PartDescription) -> tuple[int, int, StopReason]:
    return sau_loop(u575, False)


def test_a_sau_write_faults_the_next_fetch_of_the_page_it_denies(
    parked_sau_loop: tuple[int, int, StopReason],
) -> None:
    """B3.15: a Secure fetch from Non-secure memory faults. The SAU write ends the engine run
    after the store, so the fault comes at the next instruction, not at the run's end."""
    assert parked_sau_loop == (CODE + 8, 10, StopReason.FAULT)


@pytest.mark.parametrize("repeat", range(20))
def test_a_sau_write_faults_at_the_same_instruction_parked_and_running(
    u575: popcorn.PartDescription,
    parked_sau_loop: tuple[int, int, StopReason],
    repeat: int,
) -> None:
    del repeat
    assert sau_loop(u575, True) == parked_sau_loop


def test_a_tzen_1_save_is_refused_by_a_tzen_0_machine_with_the_same_config_digest(
    u575: popcorn.PartDescription,
) -> None:
    """R-04 n7: the save records TrustZone and the IDAU, a restore across them is refused before
    anything changes, key trustzone."""
    secure = popcorn.Machine(
        u575, image=popcorn.Image.elf(RCC_ELF), option_bytes=OptionBytes(tzen=True)
    )
    plain = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), option_bytes=OptionBytes())
    secure.reset()
    plain.reset()
    theirs = Snapshot.from_bytes(secure.snapshot().to_bytes())
    own = Snapshot.from_bytes(plain.snapshot().to_bytes())
    pc = plain.pc
    with pytest.raises(popcorn.ConfigError) as caught:
        plain.restore(theirs)
    assert caught.value.key == "trustzone"
    assert plain.pc == pc
    plain.reg.pc = pc + 0x40
    plain.restore(own)
    assert plain.pc == pc

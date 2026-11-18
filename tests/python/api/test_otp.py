"""The OTP areas of the STM32U5 and STM32L4: erased at power-on, written by the host, programmed
once by a CPU store through the FLASH interface."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import Machine, StopReason
from popcorn.stm32 import mcu, shipped_parts
from popcorn.units import us

OTP = 0x0BFA_0000
OTP_ALIAS = 0x0FFA_0000
OTP_AREA = 0x200
OTP_REGION = 0x400
ERASED = 0xFF

L4_OTP = 0x1FFF_7000
L4_LINES = {
    "L471": "STM32L471RETx",
    "L475": "STM32L475RCTx",
    "L476": "STM32L476RCTx",
    "L485": "STM32L485JCYx",
    "L486": "STM32L486RGTx",
}
ECCD = 1 << 31
KEY1 = 0x4567_0123
KEY2 = 0xCDEF_89AB

PERIPH = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
U5_FLASH_ELF = PERIPH / "flash_ll.elf"
L4_FLASH_ELF = PERIPH / "l476" / "l4_flash_ll.elf"


def register(description: popcorn.PartDescription, name: str) -> int:
    unit = next(p for p in description.peripherals if p.name == "FLASH")
    return unit.base + unit.register(name).offset


def booted(description: popcorn.PartDescription, elf: Path) -> Machine:
    m = Machine(description, image=popcorn.Image.elf(elf))
    m.reset()
    assert asyncio.run(m.run(budget=1_000_000)).reason is StopReason.USER
    return m


def unlock(m: Machine, description: popcorn.PartDescription, keys: str, control: str) -> None:
    m.mem.u32[register(description, keys)] = KEY1
    m.mem.u32[register(description, keys)] = KEY2
    assert m.mem.u32[register(description, control)] >> 31 == 0


def copy(m: Machine, to: int, source: int, count: int, interrupts: bool = False) -> None:
    result = asyncio.run(
        m.call("copy_words", to, source, count, budget=100_000, interrupts=interrupts)
    )
    assert result.stop.reason is StopReason.USER


def test_otp_reads_erased_and_the_secure_alias_shares_it(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575)
    assert m.mem.read(OTP, 0x200) == b"\xff" * 0x200
    assert m.mem.read(OTP_ALIAS, 16) == b"\xff" * 16
    m.mem.write(OTP + 4, b"\x12\x34")
    assert m.mem.read(OTP, 8) == bytes.fromhex("ffffffff1234ffff")
    assert m.mem.read(OTP_ALIAS, 8) == bytes.fromhex("ffffffff1234ffff")


@pytest.mark.parametrize("part", ["STM32U575QGIx", "STM32U585AIIx"])
def test_a_cpu_store_to_otp_without_pg_sets_pgserr_and_a_program_with_pg_works(part: str) -> None:
    """RM0456 7.3.7 and 7.3.9: a store with PG clear is PGSERR and writes nothing, with PG set a
    quad-word programs, the alias and the 0x200 to 0x3FF tail of the region read as erased."""
    description = mcu(part)
    m = booted(description, U5_FLASH_ELF)
    words = m.sym("words")
    for index, value in enumerate((0x11, 0x22, 0x33, 0x44)):
        m.mem.u32[words + 4 * index] = value
    for address in (OTP, OTP_ALIAS):
        copy(m, address, words, 4)
        assert m.periph.FLASH.FLASH_NSSR.PGSERR == 1
        assert m.mem.read(OTP, OTP_REGION) == b"\xff" * OTP_REGION
        m.mem.u32[register(description, "FLASH_NSSR")] = 0xFFFF_FFFF
    unlock(m, description, "FLASH_NSKEYR", "FLASH_NSCR")
    m.mem.u32[register(description, "FLASH_NSCR")] = 1
    copy(m, OTP, words, 4)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    expected = b"".join(v.to_bytes(4, "little") for v in (0x11, 0x22, 0x33, 0x44))
    assert m.mem.read(OTP, 16) == expected
    assert m.mem.read(OTP_ALIAS, 16) == expected
    assert m.mem.read(OTP + OTP_AREA, OTP_REGION - OTP_AREA) == b"\xff" * (OTP_REGION - OTP_AREA)
    assert m.periph.FLASH.FLASH_NSSR.PROGERR == 0


@pytest.mark.parametrize("address", [OTP, OTP_ALIAS])
def test_a_host_write_is_a_plain_write_and_a_later_program_of_it_sets_progerr(
    address: int,
) -> None:
    """Host provisioning stays a memory write with no FLASH flag. A firmware load returns it and a
    program of the same quad-word is PROGERR, RM0456 Table 71."""
    description = mcu("STM32U575QGIx")
    m = booted(description, U5_FLASH_ELF)
    words = m.sym("words")
    m.mem.write(address + 0x14, b"\x12\x34")
    assert m.periph.FLASH.FLASH_NSSR.PROGERR == 0
    copy(m, words, OTP + 0x14, 1)
    assert m.mem.u32[words] & 0xFFFF == 0x3412
    for index, value in enumerate((1, 2, 3, 4)):
        m.mem.u32[words + 4 * index] = value
    unlock(m, description, "FLASH_NSKEYR", "FLASH_NSCR")
    m.mem.u32[register(description, "FLASH_NSCR")] = 1
    copy(m, OTP + 0x10, words, 4)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    assert m.periph.FLASH.FLASH_NSSR.PROGERR == 1
    assert m.mem.read(OTP + 0x10, 8) == bytes.fromhex("ffffffff1234ffff")


@pytest.mark.parametrize("line", sorted(L4_LINES))
def test_an_l4_otp_double_word_programs_once_and_a_torn_read_reports_the_otp_offset(
    line: str,
) -> None:
    """RM0351 3.3.1 and 3.3.2, every shipped DIE415 line: a double word programs once, a second
    program is PROGERR, a power cut tears the next one and a CPU load of it sets ECCD with BK_ECC 0
    and ADDR_ECC the byte offset from the OTP base, inferred."""
    description = mcu(L4_LINES[line])
    m = booted(description, L4_FLASH_ELF)
    words = m.sym("words")
    m.mem.u32[words] = 0x1
    m.mem.u32[words + 4] = 0x2
    unlock(m, description, "KEYR", "CR")
    m.mem.u32[register(description, "CR")] = 1
    copy(m, L4_OTP + 8, words, 2)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    assert m.periph.FLASH.SR.PROGERR == 0
    assert m.mem.read(L4_OTP + 8, 8) == bytes.fromhex("0100000002000000")
    copy(m, L4_OTP + 8, words, 2)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    assert m.periph.FLASH.SR.PROGERR == 1
    m.mem.u32[register(description, "SR")] = 0xFFFF_FFFF
    copy(m, L4_OTP + 0x10, words, 2)
    assert m.periph.FLASH.SR.BSY == 1
    assert asyncio.run(m.run(for_=us(40))).reason is StopReason.USER
    assert m.periph.FLASH.SR.BSY == 1
    m.power_cycle()
    assert m.mem.read(L4_OTP + 0x10, 8) == bytes.fromhex("01000000ffffffff")
    copy(m, words, L4_OTP + 0x14, 1, interrupts=True)
    assert m.mem.u32[m.sym("nmi_count")] == 1
    assert m.mem.u32[m.sym("nmi_eccr")] == ECCD | 0x10


def test_no_l1_part_has_an_otp_region() -> None:
    """RM0038 has no OTP area, so no shipped L1 part describes one."""
    parts = [part for part in shipped_parts() if part.startswith("STM32L1")]
    assert len(parts) > 0
    for part in parts:
        assert not [r.name for r in mcu(part).regions if "OTP" in r.name], part

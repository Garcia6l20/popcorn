"""The OTP area of the STM32H5: provisioned by the host, programmed in 16-bit units by a CPU store
through the FLASH interface, RM0481 7.3.9."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import Machine, StopReason
from popcorn.stm32 import mcu

OTP = 0x08FF_F000
KEY1 = 0x4567_0123
KEY2 = 0xCDEF_89AB
PG = 1 << 1
H5_PARTS = ["STM32H562AGIx", "STM32H563AIIxQ", "STM32H573IITxQ"]
FLASH_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "h573" / "flash_ll.elf"


def register(description: popcorn.PartDescription, name: str) -> int:
    unit = next(p for p in description.peripherals if p.name == "FLASH")
    return unit.base + unit.register(name).offset


def booted(part: str) -> tuple[popcorn.PartDescription, Machine]:
    description = mcu(part)
    m = Machine(description, image=popcorn.Image.elf(FLASH_ELF))
    m.reset()
    assert asyncio.run(m.run(budget=1_000_000)).reason is StopReason.USER
    return description, m


def copy(m: Machine, to: int, source: int, count: int) -> None:
    result = asyncio.run(m.call("copy_words", to, source, count, budget=100_000))
    assert result.stop.reason is StopReason.USER


@pytest.mark.parametrize("part", H5_PARTS)
def test_the_otp_reads_erased_and_host_provisioning_reads_back(part: str) -> None:
    _, m = booted(part)
    assert m.mem.read(OTP, 0x800) == b"\xff" * 0x800
    m.mem.write(OTP + 4, b"\x34\x12")
    assert m.mem.read(OTP, 8) == bytes.fromhex("ffffffff3412ffff")
    m.mem.write(OTP + 8, b"\xff\xff")
    assert m.mem.read(OTP + 8, 2) == b"\xff\xff"


@pytest.mark.parametrize("part", H5_PARTS)
def test_a_cpu_store_to_the_otp_without_pg_sets_pgserr_and_a_program_with_pg_works(
    part: str,
) -> None:
    """RM0481 7.9.4 and 7.3.9: a store with PG clear is PGSERR and writes nothing, with PG set a
    word store programs two half-words."""
    description, m = booted(part)
    words = m.sym("words")
    m.mem.u32[words] = 0x5678_1234
    copy(m, OTP, words, 1)
    assert m.periph.FLASH.FLASH_NSSR.PGSERR == 1
    assert m.mem.read(OTP, 8) == b"\xff" * 8
    m.mem.u32[register(description, "FLASH_NSKEYR")] = KEY1
    m.mem.u32[register(description, "FLASH_NSKEYR")] = KEY2
    m.mem.u32[register(description, "FLASH_NSCCR")] = 0xFFFF_FFFF
    m.mem.u32[register(description, "FLASH_NSCR")] = PG
    copy(m, OTP, words, 1)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    assert m.mem.read(OTP, 8) == bytes.fromhex("34127856ffffffff")
    assert m.periph.FLASH.FLASH_NSSR.PGSERR == 0


@pytest.mark.parametrize("part", H5_PARTS)
def test_a_locked_otp_block_refuses_a_cpu_store_with_wrperr(part: str) -> None:
    """RM0481 7.11.29 and 7.11.30: a PRG bit latched by OPTSTRT locks its 64-byte block."""
    description, m = booted(part)
    words = m.sym("words")
    m.mem.u32[words] = 0x5678_1234
    m.mem.u32[register(description, "FLASH_NSKEYR")] = KEY1
    m.mem.u32[register(description, "FLASH_NSKEYR")] = KEY2
    m.mem.u32[register(description, "FLASH_OPTKEYR")] = 0x0819_2A3B
    m.mem.u32[register(description, "FLASH_OPTKEYR")] = 0x4C5D_6E7F
    m.mem.u32[register(description, "FLASH_OTPBLR_PRG")] = 1 << 5
    assert m.periph.FLASH.FLASH_OTPBLR_CUR.LOCKBL == 0
    m.mem.u32[register(description, "FLASH_OPTCR")] = 1 << 1
    assert m.periph.FLASH.FLASH_OTPBLR_CUR.LOCKBL == 1 << 5
    m.mem.u32[register(description, "FLASH_NSCR")] = PG
    copy(m, OTP + 5 * 64, words, 1)
    assert m.periph.FLASH.FLASH_NSSR.WRPERR == 1
    assert m.mem.read(OTP + 5 * 64, 4) == b"\xff" * 4
    m.mem.u32[register(description, "FLASH_NSCCR")] = 0xFFFF_FFFF
    copy(m, OTP + 6 * 64, words, 1)
    assert asyncio.run(m.call("wait_idle", budget=1_000_000)).stop.reason is StopReason.USER
    assert m.periph.FLASH.FLASH_NSSR.WRPERR == 0
    assert m.mem.read(OTP + 6 * 64, 4) == bytes.fromhex("34127856")

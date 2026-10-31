"""ST's HAL CRC driver on the STM32L1 CRC unit, checked against a bit-serial reference.

The STM32L1 CRC has one fixed configuration, RM0038 section 4: the polynomial 0x04C11DB7 from
0xFFFFFFFF on 32-bit words, with no reversal and no final XOR, CRC-32/MPEG-2. The firmware runs
`HAL_CRC_Init`, whose `HAL_CRC_MspInit` sets RCC_AHBENR.CRCEN, then `HAL_CRC_Calculate` over
0x12345678, `HAL_CRC_Accumulate` over 0x9ABCDEF0 on top, and `HAL_CRC_Calculate` over an 8-word
block, which resets CRC_DR first. Then it stores a byte in CRC_IDR and reads it back, and stops at
`computed`. The project sets `MachineDescription(strict=True)`: any register access the CRC model
does not serve fails the run."""

import struct

import pytest
from popcorn import Machine, StopReason

pytestmark = pytest.mark.popcorn(budget=1_000_000)

POLY = 0x04C11DB7
INIT = 0xFFFFFFFF
BLOCK = (
    0x00000000,
    0xFFFFFFFF,
    0x01234567,
    0x89ABCDEF,
    0xDEADBEEF,
    0x0BADF00D,
    0x80000001,
    0x7FFFFFFE,
)


def mpeg_2_bytes(data: bytes) -> int:
    crc = INIT
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ POLY) if crc & 0x8000_0000 else crc << 1
            crc &= 0xFFFF_FFFF
    return crc


def mpeg_2(words: tuple[int, ...]) -> int:
    """The unit takes each CRC_DR word most significant byte first."""
    return mpeg_2_bytes(struct.pack(f">{len(words)}I", *words))


async def compute(machine: Machine) -> None:
    stop = await machine.run(until="computed")
    assert stop.reason is StopReason.ADDRESS, stop


def test_the_reference_gives_the_catalogue_check_value() -> None:
    assert mpeg_2_bytes(b"123456789") == 0x0376E6E7


async def test_one_word_and_an_accumulated_word_match_crc_32_mpeg_2(crc: Machine) -> None:
    await compute(crc)
    assert crc.var("crc_first").value == mpeg_2((0x12345678,))
    assert crc.var("crc_accumulated").value == mpeg_2((0x12345678, 0x9ABCDEF0))


async def test_a_calculate_restarts_from_the_initial_value(crc: Machine) -> None:
    await compute(crc)
    assert crc.var("crc_block").value == mpeg_2(BLOCK)
    assert crc.periph.CRC.DR.value == mpeg_2(BLOCK)


async def test_the_msp_init_enables_the_crc_clock(crc: Machine) -> None:
    assert crc.periph.RCC.AHBENR.CRCEN == 0
    await compute(crc)
    assert crc.periph.RCC.AHBENR.CRCEN == 1


async def test_the_independent_data_register_keeps_a_byte(crc: Machine) -> None:
    await compute(crc)
    assert crc.var("tag").value == 0xA5
    assert crc.periph.CRC.IDR.value == 0xA5

"""ST's HAL CRC driver computes three CRCs on the CRC unit, checked against a bit-serial reference.

The firmware runs `HAL_CRC_Init` three times, then stops at `computed`:

- the reset configuration on 32-bit words, CRC-32/MPEG-2: `HAL_CRC_Calculate` over 0x12345678,
  then `HAL_CRC_Accumulate` over 0x9ABCDEF0;
- input reversed by byte and output reversed, on the bytes "123456789", complemented in software:
  CRC-32/ISO-HDLC, the zlib CRC;
- a 16-bit polynomial 0x1021 from 0xFFFF on the same bytes: CRC-16/IBM-3740.

The HAL feeds a byte buffer four bytes per word write, then a half-word and a byte write for the
tail, so the last two CRCs also cover sub-word CRC_DR writes. The project sets
`MachineDescription(strict=True)`: any register access the CRC model does not serve fails the run.
"""

import logging
from dataclasses import dataclass

import pytest
from popcorn import Machine, StopReason

pytestmark = pytest.mark.popcorn(budget=1_000_000)

CHECK_INPUT = b"123456789"
WORDS = (0x12345678, 0x9ABCDEF0)


@dataclass(frozen=True)
class CrcModel:
    """CRC parameters in the Rocksoft model the CRC catalogue uses."""

    width: int
    poly: int
    init: int
    refin: bool
    refout: bool
    xorout: int


MPEG_2 = CrcModel(32, 0x04C11DB7, 0xFFFFFFFF, False, False, 0)
ISO_HDLC = CrcModel(32, 0x04C11DB7, 0xFFFFFFFF, True, True, 0xFFFFFFFF)
IBM_3740 = CrcModel(16, 0x1021, 0xFFFF, False, False, 0)


def reflect(value: int, bits: int) -> int:
    return int(f"{value:0{bits}b}"[::-1], 2)


def reference(model: CrcModel, data: bytes) -> int:
    top = 1 << (model.width - 1)
    mask = (1 << model.width) - 1
    crc = model.init
    for byte in data:
        crc ^= (reflect(byte, 8) if model.refin else byte) << (model.width - 8)
        for _ in range(8):
            crc = ((crc << 1) ^ model.poly) if crc & top else crc << 1
            crc &= mask
    if model.refout:
        crc = reflect(crc, model.width)
    return crc ^ model.xorout


def big_endian(*words: int) -> bytes:
    return b"".join(word.to_bytes(4, "big") for word in words)


async def results(machine: Machine) -> dict[str, int]:
    stop = await machine.run(until="computed")
    assert stop.reason is StopReason.ADDRESS, stop
    seen = machine.var("results")
    names = ("one_word", "two_words", "iso_hdlc", "ibm_3740", "errors")
    return {name: int(getattr(seen, name).value) for name in names}


def test_the_reference_matches_the_catalogue_check_values() -> None:
    assert reference(MPEG_2, CHECK_INPUT) == 0x0376E6E7
    assert reference(ISO_HDLC, CHECK_INPUT) == 0xCBF43926
    assert reference(IBM_3740, CHECK_INPUT) == 0x29B1


async def test_the_hal_inits_without_error_and_nothing_is_logged(
    machine: Machine, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        assert (await results(machine))["errors"] == 0
    assert [record.getMessage() for record in caplog.records] == []


async def test_words_in_the_reset_configuration_compute_crc32_mpeg2(machine: Machine) -> None:
    seen = await results(machine)
    assert seen["one_word"] == reference(MPEG_2, big_endian(WORDS[0])) == 0xDF8A8A2B
    assert seen["two_words"] == reference(MPEG_2, big_endian(*WORDS)) == 0x7D24A31B


async def test_reversed_bytes_compute_crc32_iso_hdlc(machine: Machine) -> None:
    seen = await results(machine)
    assert seen["iso_hdlc"] == reference(ISO_HDLC, CHECK_INPUT) == 0xCBF43926


async def test_a_16_bit_polynomial_computes_crc16_ibm_3740(machine: Machine) -> None:
    seen = await results(machine)
    assert seen["ibm_3740"] == reference(IBM_3740, CHECK_INPUT) == 0x29B1

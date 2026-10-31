"""A HAL boot counter in data EEPROM and a program-memory page erase and program, checked from
the host, on the xe and the xb build.

The firmware raises the clock to 32 MHz from HSI and the PLL in voltage range 1 with one flash
wait state, RM0038 Table 13. It reads a boot counter from the first data EEPROM word, writes it
back incremented with `HAL_FLASHEx_DATAEEPROM_Program`, then erases the 256-byte page holding
the `data_page` array with `HAL_FLASHEx_Erase` and programs a 4-word record into it with
`HAL_FLASH_Program`. An erased STM32L1 flash word reads 0, RM0038 3.4.2. The PECR unlock
sequences, PELOCK and PRGLOCK, run through the FLASH model.
"""

import struct

import pytest
from popcorn import Machine, ModelError, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=2_000_000)

EEPROM = 0x0808_0000
PAGE_BYTES = 256
RECORD = struct.pack("<4I", 0xC0FFEE00, 0x12345678, 0x9ABCDEF0, 0x0BADF00D)
NO_PAGE_ERROR = 0xFFFF_FFFF


async def run_to(machine: Machine, marker: str) -> None:
    stop = await machine.run(until=marker, for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop


def page(machine: Machine) -> int:
    return machine.sym("data_page")


async def test_range_1_and_one_wait_state_carry_32_mhz(pwr_flash: Machine) -> None:
    await run_to(pwr_flash, "clock_configured")
    assert pwr_flash.var("SystemCoreClock").value == 32_000_000
    assert pwr_flash.periph.PWR.CR.VOS == 1
    acr = pwr_flash.periph.FLASH.ACR
    assert (acr.ACC64, acr.LATENCY) == (1, 1)


async def test_the_boot_counter_starts_at_one_on_an_erased_eeprom(pwr_flash: Machine) -> None:
    assert pwr_flash.mem.u32[EEPROM] == 0
    await run_to(pwr_flash, "eeprom_counted")
    assert pwr_flash.var("boot_count").value == 1
    assert pwr_flash.mem.u32[EEPROM] == 1


async def test_the_boot_counter_counts_on_from_the_stored_value(pwr_flash: Machine) -> None:
    pwr_flash.mem.u32[EEPROM] = 41
    await run_to(pwr_flash, "eeprom_counted")
    assert pwr_flash.mem.u32[EEPROM] == 42
    assert pwr_flash.periph.FLASH.PECR.PELOCK == 1


async def test_the_page_reads_erased_then_programmed(pwr_flash: Machine) -> None:
    start = page(pwr_flash)
    assert start % PAGE_BYTES == 0
    assert pwr_flash.mem.read(start, 8) != bytes(8)

    await run_to(pwr_flash, "page_erased")
    assert pwr_flash.var("erase_status").value == 0
    assert pwr_flash.var("page_error").value == NO_PAGE_ERROR
    assert pwr_flash.mem.read(start, PAGE_BYTES) == bytes(PAGE_BYTES)

    await run_to(pwr_flash, "flash_done")
    assert pwr_flash.var("program_status").value == 0
    assert pwr_flash.mem.read(start, PAGE_BYTES) == RECORD + bytes(PAGE_BYTES - len(RECORD))
    pecr = pwr_flash.periph.FLASH.PECR
    assert (pecr.PELOCK, pecr.PRGLOCK) == (0, 1), "HAL_FLASH_Lock sets PRGLOCK only"


async def test_no_wait_state_at_32_mhz_fails_a_strict_run(pwr_flash: Machine) -> None:
    """The ModelError ends the call with the interrupted context restored, as a return does."""
    await run_to(pwr_flash, "flash_done")
    pc, sp = pwr_flash.pc, pwr_flash.reg.sp
    with pytest.raises(ModelError, match="FLASH"):
        await pwr_flash.call("set_latency", 0, budget=1_000)
    assert (pwr_flash.pc, pwr_flash.reg.sp) == (pc, sp)

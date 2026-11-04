"""A HAL page erase and double-word program in bank 2 and an option byte read, checked from the
host.

The firmware raises the clock to 80 MHz from HSI16 and the PLL in voltage range 1, the PWR_CR1
reset range, with four flash wait states, RM0351 Table 11. It erases the last 2-Kbyte page of
bank 2 with `HAL_FLASHEx_Erase`, programs two double words into it with `HAL_FLASH_Program`,
RM0351 3.3.6 and 3.3.7. The firmware derives that page from the flash size, PNB
FLASH_BANK_SIZE / 2 Kbytes - 1 with BKER, at the flash end minus 2 Kbytes, RM0351 Table 10:
PNB 63, page 319 at 0x0803F800 on the 256-Kbyte part. Then it reads the option bytes with
`HAL_FLASHEx_OBGetConfig`. An erased STM32L4 flash word reads 0xFFFFFFFF. BSY lasts the DS10198
times the FLASH model takes: 22.02 ms for the page erase and 81.69 us for each double word.
"""

import struct

import pytest
from popcorn import Call, Machine, ModelError, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=4_000_000)

FLASH_BASE = 0x0800_0000
PAGE_BYTES = 2048
RECORD = struct.pack("<2Q", 0x12345678C0FFEE00, 0x0BADF00D9ABCDEF0)
ERASED = b"\xff" * PAGE_BYTES
RDP_LEVEL_0 = 0xAA
OPTR_PRODUCTION = 0xFFEFF8AA
USER_PRODUCTION = OPTR_PRODUCTION & ~0xFF
AROUND_BSY = us(5)


async def run_to(machine: Machine, marker: str) -> None:
    stop = await machine.run(until=marker, for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop


def flash_bytes(machine: Machine) -> int:
    return next(r.size for r in machine.description.regions if r.name == "Flash")


def last_page(machine: Machine) -> int:
    return FLASH_BASE + flash_bytes(machine) - PAGE_BYTES


def timestamps(machine: Machine, *functions: str) -> dict[str, VirtualTime]:
    seen: dict[str, VirtualTime] = {}
    for function in functions:

        def record(call: Call, function: str = function) -> Return:
            seen[function] = machine.time
            return call.proceed()

        machine.intercept(function, record)
    return seen


async def test_range_1_and_four_wait_states_carry_80_mhz(pwr_flash: Machine) -> None:
    await run_to(pwr_flash, "clock_configured")
    assert pwr_flash.var("SystemCoreClock").value == 80_000_000
    assert pwr_flash.periph.PWR.CR1.VOS == 1
    assert pwr_flash.periph.FLASH.ACR.LATENCY == 4


async def test_the_page_reads_erased_then_programmed(pwr_flash: Machine) -> None:
    page = last_page(pwr_flash)
    pwr_flash.mem.write(page, bytes(32))

    await run_to(pwr_flash, "page_erased")
    assert pwr_flash.var("data_address").value == page
    assert pwr_flash.var("data_page").value == flash_bytes(pwr_flash) // 2 // PAGE_BYTES - 1
    assert pwr_flash.var("erase_status").value == 0
    assert pwr_flash.var("page_error").value == 0xFFFF_FFFF
    assert pwr_flash.mem.read(page, PAGE_BYTES) == ERASED

    await run_to(pwr_flash, "flash_done")
    assert pwr_flash.var("program_status").value == 0
    assert pwr_flash.mem.read(page, PAGE_BYTES) == RECORD + ERASED[len(RECORD) :]
    assert pwr_flash.periph.FLASH.CR.LOCK == 1


async def test_bsy_lasts_the_datasheet_erase_and_program_times(pwr_flash: Machine) -> None:
    seen = timestamps(pwr_flash, "clock_configured", "page_erased", "flash_done")
    await run_to(pwr_flash, "options_read")

    erase = seen["page_erased"] - seen["clock_configured"]
    program = seen["flash_done"] - seen["page_erased"]
    assert us(22_020).ps <= erase.ps < (us(22_020) + AROUND_BSY).ps
    assert (2 * us(81.69)).ps <= program.ps < (2 * us(81.69) + AROUND_BSY).ps


async def test_the_option_bytes_read_their_production_values(pwr_flash: Machine) -> None:
    await run_to(pwr_flash, "options_read")
    assert pwr_flash.var("rdp_level").value == RDP_LEVEL_0
    assert pwr_flash.var("user_config").value == USER_PRODUCTION
    assert pwr_flash.periph.FLASH.OPTR.value == OPTR_PRODUCTION
    assert pwr_flash.var("flash_kb").value == flash_bytes(pwr_flash) // 1024


async def test_no_wait_state_at_80_mhz_fails_a_strict_run(pwr_flash: Machine) -> None:
    """The ModelError ends the call with the interrupted context restored, as a return does."""
    await run_to(pwr_flash, "options_read")
    pc, sp = pwr_flash.pc, pwr_flash.reg.sp
    with pytest.raises(ModelError, match="FLASH"):
        await pwr_flash.call("set_latency", 0, budget=1_000)
    assert (pwr_flash.pc, pwr_flash.reg.sp) == (pc, sp)

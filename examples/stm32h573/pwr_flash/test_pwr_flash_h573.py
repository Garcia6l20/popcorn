"""An LL flash sector erase and record program, checked from the host.

The firmware raises the clock to 250 MHz in voltage range VOS0 with 5 flash wait states, erases
the last sector of bank 2 with FLASH_NSCR.SER, SNB and BKSEL, programs one quad-word with PG and
four word stores, then stores into flash once more without PG. The FLASH model serves those
stores: BSY lasts the datasheet times in virtual time, EOP stays set until FLASH_NSCCR.CLR_EOP
clears it, the sector reads erased then programmed through `machine.mem`, and the stray store
sets PGSERR without touching memory.

Each BSY time is measured between two marker calls, so it also counts the code around the BSY
poll: under 250 instructions at 250 MHz and a CPI of 1, which is 1 us.
"""

import struct

import pytest
from popcorn import Call, Machine, ModelError, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

DATA_SECTOR = 0x081F_E000
RECORD = struct.pack("<4I", 0xC0FFEE00, 0x12345678, 0x9ABCDEF0, 0x0BADF00D)
ERASED = b"\xff" * 16
AROUND_BSY = us(1)
PACKAGE_BASE = 0x08FF_F80E
PACKAGE_MASK = 0x3F
UID_BASE = 0x08FF_F800
POPCORN_UID = (0x0032_0014, 0x4F52_4E01, 0x504F_5043)


async def run_to(machine: Machine, marker: str) -> None:
    stop = await machine.run(until=marker, for_=ms(20))
    assert stop.reason is StopReason.ADDRESS, stop


def field_mask(machine: Machine, peripheral: str, register: str, field: str) -> int:
    unit = next(p for p in machine.description.peripherals if p.name == peripheral)
    reg = unit.register(register)
    bits = next(f for f in reg.fields if f.name == field)
    return ((1 << bits.width) - 1) << bits.lsb


def timestamps(machine: Machine, *functions: str) -> dict[str, VirtualTime]:
    seen: dict[str, VirtualTime] = {}
    for function in functions:

        def record(call: Call, function: str = function) -> Return:
            seen[function] = machine.time
            return call.proceed()

        machine.intercept(function, record)
    return seen


async def test_the_sector_reads_erased_then_programmed(pwr_flash: Machine) -> None:
    pwr_flash.mem.write(DATA_SECTOR, b"\x00" * 32)

    await run_to(pwr_flash, "sector_erased")
    assert pwr_flash.var("erase_status").value == 0
    assert pwr_flash.mem.read(DATA_SECTOR, 32) == ERASED * 2

    await run_to(pwr_flash, "record_programmed")
    assert pwr_flash.var("program_status").value == 0
    assert pwr_flash.mem.read(DATA_SECTOR, 16) == RECORD
    assert pwr_flash.mem.read(DATA_SECTOR + 16, 16) == ERASED


async def test_eop_is_set_without_an_interrupt_and_cleared_through_nsccr(
    pwr_flash: Machine,
) -> None:
    await run_to(pwr_flash, "sector_erased")
    flash = pwr_flash.periph.FLASH
    eop = field_mask(pwr_flash, "FLASH", "FLASH_NSSR", "EOP")
    assert pwr_flash.var("erase_end_flag").value == eop
    assert (flash.FLASH_NSCR.EOPIE, flash.FLASH_NSSR.EOP) == (0, 1)

    flash.FLASH_NSSR.EOP = 1
    assert flash.FLASH_NSSR.EOP == 1
    flash.FLASH_NSCCR.CLR_EOP = 1
    assert flash.FLASH_NSSR.EOP == 0


async def test_bsy_lasts_the_datasheet_erase_and_program_times(pwr_flash: Machine) -> None:
    seen = timestamps(pwr_flash, "clock_configured", "sector_erased", "record_programmed")
    await run_to(pwr_flash, "flash_done")

    erase = seen["sector_erased"] - seen["clock_configured"]
    program = seen["record_programmed"] - seen["sector_erased"]
    assert us(2000).ps <= erase.ps < (us(2000) + AROUND_BSY).ps
    assert us(31).ps <= program.ps < (us(31) + AROUND_BSY).ps


async def test_a_store_without_pg_sets_pgserr_and_leaves_flash_unchanged(
    pwr_flash: Machine,
) -> None:
    await run_to(pwr_flash, "flash_done")

    assert pwr_flash.var("stray_status").value == field_mask(
        pwr_flash, "FLASH", "FLASH_NSSR", "PGSERR"
    )
    status = pwr_flash.periph.FLASH.FLASH_NSSR
    assert (status.PGSERR, status.BSY, status.WBNE) == (1, 0, 0)
    assert pwr_flash.periph.FLASH.FLASH_NSCR.LOCK == 1
    assert pwr_flash.mem.read(DATA_SECTOR, 32) == RECORD + ERASED


async def test_too_few_wait_states_at_250_mhz_fail_a_strict_run(pwr_flash: Machine) -> None:
    """The ModelError ends the call with the interrupted context restored, as a return does. The
    store that raised it stays: FLASH_ACR.LATENCY reads 1 afterwards."""
    await run_to(pwr_flash, "flash_done")
    assert pwr_flash.periph.FLASH.FLASH_ACR.LATENCY == 5
    assert pwr_flash.periph.FLASH.FLASH_ACR.WRHIGHFREQ == 2
    pc, sp = pwr_flash.pc, pwr_flash.reg.sp

    with pytest.raises(ModelError, match="FLASH"):
        await pwr_flash.call("set_latency", 1, budget=1_000)
    assert (pwr_flash.pc, pwr_flash.reg.sp) == (pc, sp)
    assert pwr_flash.periph.FLASH.FLASH_ACR.LATENCY == 1


async def test_the_firmware_reads_the_device_signature(pwr_flash: Machine) -> None:
    await run_to(pwr_flash, "signature_read")

    signature = pwr_flash.var("signature")
    assert signature.flash_kb.value == 0x800
    assert signature.package.value == pwr_flash.mem.u16[PACKAGE_BASE] & PACKAGE_MASK
    uid = tuple(int(signature.uid[word].value) for word in range(3))
    assert uid == struct.unpack("<3I", pwr_flash.mem.read(UID_BASE, 12))
    assert uid == POPCORN_UID

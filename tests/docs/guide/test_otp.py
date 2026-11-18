from pathlib import Path

import pytest

PREBUILT = Path(__file__).parents[3] / "tests/firmware/periph/prebuilt"


def test_the_host_provisions_the_otp_with_a_plain_write() -> None:
    # --8<-- [start:provision]
    import popcorn
    from popcorn.stm32 import mcu

    machine = popcorn.Machine(mcu("STM32U575QGIx"))
    machine.reset()
    OTP = 0x0BFA_0000
    assert machine.mem.read(OTP, 4) == b"\xff\xff\xff\xff"

    machine.mem.write(OTP + 0x10, bytes(range(16)))
    assert machine.mem.read(OTP + 0x10, 16) == bytes(range(16))
    assert machine.mem.read(0x0FFA_0010, 4) == bytes(range(4))
    # --8<-- [end:provision]


async def test_a_u5_firmware_programs_a_quad_word_once(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.chdir(PREBUILT)
    # --8<-- [start:u5]
    import popcorn
    from popcorn.stm32 import mcu

    description = mcu("STM32U575QGIx")
    machine = popcorn.Machine(description, image=popcorn.Image.elf("flash_ll.elf"))
    machine.reset()
    await machine.run(until="main", budget=1_000_000)

    flash = next(p for p in description.peripherals if p.name == "FLASH")
    nscr = flash.base + flash.register("FLASH_NSCR").offset
    nskeyr = flash.base + flash.register("FLASH_NSKEYR").offset
    OTP = 0x0BFA_0000
    words = machine.sym("words")
    for index, value in enumerate((1, 2, 3, 4)):
        machine.mem.u32[words + 4 * index] = value

    machine.mem.u32[nskeyr] = 0x4567_0123
    machine.mem.u32[nskeyr] = 0xCDEF_89AB
    machine.mem.u32[nscr] = 1  # PG
    await machine.call("copy_words", OTP, words, 4)
    await machine.call("wait_idle")
    assert machine.mem.read(OTP, 4) == (1).to_bytes(4, "little")
    assert machine.periph.FLASH.FLASH_NSSR.PROGERR == 0

    await machine.call("copy_words", OTP, words, 4)
    await machine.call("wait_idle")
    assert machine.periph.FLASH.FLASH_NSSR.PROGERR == 1
    # --8<-- [end:u5]


async def test_an_h5_virgin_read_is_an_ecc_error_the_firmware_can_mask(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.chdir(PREBUILT / "h573")
    # --8<-- [start:h5]
    import popcorn
    from popcorn import StopReason
    from popcorn.stm32 import mcu

    description = mcu("STM32H573IITxQ")
    machine = popcorn.Machine(description, image=popcorn.Image.elf("flash_ll.elf"))
    machine.reset()
    stop = await machine.run(until="main", budget=1_000_000)
    assert stop.reason is StopReason.ADDRESS

    sbs = next(p for p in description.peripherals if p.name == "SBS")
    OTP = 0x08FF_F000
    words = machine.sym("words")
    machine.periph.RCC.RCC_APB3ENR.SBSEN = 1
    machine.mem.u32[sbs.base + sbs.register("SBS_ECCNMIR").offset] = 1

    await machine.call("copy_words", words, OTP + 0x20, 1, interrupts=True)
    assert machine.periph.FLASH.FLASH_ECCDETR.ECCD == 1
    assert machine.periph.FLASH.FLASH_ECCDETR.ADDR_ECC == 0x702
    assert machine.periph.FLASH.FLASH_ECCDR.DATA_ECC == 0xFFFF
    # --8<-- [end:h5]

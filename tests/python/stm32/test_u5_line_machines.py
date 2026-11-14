from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from popcorn import Image, Machine, OptionBytes, StopReason
from popcorn.units import us

PARTS = [
    "STM32U535CBTx",
    "STM32U535CETx",
    "STM32U535RETx",
    "STM32U535VETx",
    "STM32U545CETx",
    "STM32U545RETx",
]
SMOKE_PARTS = ["STM32U535CETx", "STM32U545CETx"]

RCC_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
HSION = 1 << 8
ADC4EN = 1 << 5
ADCDACSEL_HSI16 = 4 << 12
ADEN = 1 << 0
ADSTART = 1 << 2
ADVREGEN = 1 << 28


@pytest.mark.parametrize("tzen", [False, True], ids=["tzen0", "tzen1"])
@pytest.mark.parametrize("part", PARTS)
def test_the_machine_of_a_u535_or_u545_part_builds(part: str, tzen: bool) -> None:
    from popcorn.stm32 import mcu

    machine = Machine(mcu(part), option_bytes=OptionBytes(tzen=tzen))
    machine.reset()


def settle(m: Machine, duration: int) -> None:
    assert asyncio.run(m.run(for_=us(duration))).reason is StopReason.TIME


def idle(part: str) -> Machine:
    from popcorn.stm32 import mcu

    m = Machine(mcu(part), image=Image.elf(RCC_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    return m


@pytest.mark.parametrize("part", SMOKE_PARTS)
def test_a_tamp_backup_register_and_the_dbgmcu_status_register_answer(part: str) -> None:
    m = idle(part)
    backup = m.periph.TAMP.BKP0R
    backup.write(0xA5A5_1234)
    assert backup.value == 0xA5A5_1234
    assert m.periph.DBGMCU.SR.value == 1


@pytest.mark.parametrize(
    ("part", "channel"),
    [
        ("STM32U535CETx", 9),
        ("STM32U545CETx", 9),
        ("STM32U535RETx", 1),
        ("STM32U545RETx", 1),
    ],
)
def test_a_driven_adc4_pad_converts(part: str, channel: int) -> None:
    m = idle(part)
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSION)
    settle(m, 260)
    assert rcc.RCC_CR.HSIRDY == 1
    rcc.RCC_AHB3ENR.write(rcc.RCC_AHB3ENR.value | ADC4EN)
    rcc.RCC_CCIPR3.write(ADCDACSEL_HSI16)
    adc = m.periph.ADC4
    adc.CR.write(ADVREGEN)
    settle(m, 30)
    adc.CR.write(ADVREGEN | ADEN)
    settle(m, 2)
    assert adc.ISR.ADRDY == 1
    adc.input(channel).volts = 1.2
    adc.CHSELRMOD0.write(1 << channel)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    settle(m, 2)
    assert (adc.ISR.EOC, adc.ISR.EOS) == (1, 1)
    assert adc.DR.value == int(1.2 * 4096 / 3.0)


@pytest.mark.parametrize("part", SMOKE_PARTS)
def test_the_adc4_temperature_sensor_select_is_writable(part: str) -> None:
    m = idle(part)
    adc = m.periph.ADC4
    adc.CCR.write(1 << 23)
    assert adc.CCR.VSENSESEL == 1


@pytest.mark.parametrize("part", SMOKE_PARTS)
def test_the_mpcbb_super_blocks_follow_the_sram_sizes_of_rm0456_table_32(part: str) -> None:
    from popcorn.stm32 import mcu

    m = Machine(mcu(part), option_bytes=OptionBytes(tzen=True))
    m.reset()
    for name, blocks in {"GTZC1_MPCBB1": 12, "GTZC1_MPCBB2": 4, "GTZC2_MPCBB4": 1}.items():
        controller = getattr(m.periph, name)
        assert getattr(controller, f"SECCFGR{blocks - 1}").value == 0xFFFF_FFFF
        if name != "GTZC2_MPCBB4":
            assert getattr(controller, f"SECCFGR{blocks}").value == 0


@pytest.mark.parametrize(
    ("part", "port", "pin", "lpgpio_pin"),
    [
        ("STM32U545RETx", "B", 0, 9),
        ("STM32U535CETx", "B", 10, 4),
        ("STM32U535VETx", "E", 3, 15),
        ("STM32U545RETx", "C", 12, 10),
    ],
)
def test_an_lpgpio1_output_drives_its_pad(part: str, port: str, pin: int, lpgpio_pin: int) -> None:
    from popcorn.stm32 import mcu

    m = Machine(mcu(part))
    m.reset()
    rcc = m.periph.RCC
    rcc.RCC_AHB2ENR1.write(rcc.RCC_AHB2ENR1.value | 1 << (ord(port) - ord("A")))
    rcc.RCC_AHB3ENR.write(rcc.RCC_AHB3ENR.value | 1)
    pad = getattr(m.periph, f"GPIO{port}")
    moder, afr = (pad.MODER, pad.AFRL if pin < 8 else pad.AFRH)
    shift = 4 * (pin % 8)
    moder.write((moder.value & ~(3 << 2 * pin)) | 2 << 2 * pin)
    afr.write((afr.value & ~(0xF << shift)) | 11 << shift)
    lp = m.periph.LPGPIO1
    lp.MODER.write(1 << lpgpio_pin)
    lp.ODR.write(1 << lpgpio_pin)
    assert pad.pin(pin).level
    lp.BRR.write(1 << lpgpio_pin)
    assert not pad.pin(pin).level


@pytest.mark.parametrize("part", SMOKE_PARTS)
def test_adc4_channel_1_reaches_no_pin_of_the_48_pin_packages(part: str) -> None:
    from popcorn import ConfigError

    m = idle(part)
    with pytest.raises(ConfigError, match="channel 1 reaches no package pin"):
        m.periph.ADC4.input(1).volts = 1.2

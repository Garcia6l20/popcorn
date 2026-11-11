"""The regulator, calibration and enable sequence of the built-in STM32U5 ADC4 model."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, StopReason
from popcorn.units import us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
RCC_ELF = PREBUILT / "rcc_ll.elf"
ADEN = 1 << 0
ADDIS = 1 << 1
ADVREGEN = 1 << 28
ADCAL = 1 << 31
HSION = 1 << 8
ADC4EN = 1 << 5
ADCDACSEL_HSI16 = 4 << 12


def settle(m: popcorn.Machine, duration: int) -> None:
    assert asyncio.run(m.run(for_=us(duration))).reason is StopReason.TIME


def idle(u575: popcorn.PartDescription, strict: bool = False) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RCC_ELF), strict_peripherals=strict)
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSION)
    settle(m, 200)
    assert rcc.RCC_CR.HSIRDY == 1
    rcc.RCC_AHB3ENR.write(rcc.RCC_AHB3ENR.value | ADC4EN)
    rcc.RCC_CCIPR3.write(ADCDACSEL_HSI16)
    return m


def test_the_regulator_calibration_and_enable_set_their_flags(
    u575: popcorn.PartDescription,
) -> None:
    m = idle(u575)
    adc = m.periph.ADC4
    adc.CR.write(ADVREGEN)
    settle(m, 30)
    assert adc.ISR.LDORDY == 1
    adc.CR.write(ADVREGEN | ADCAL)
    assert adc.CR.ADCAL == 1
    settle(m, 10)
    assert (adc.CR.ADCAL, adc.ISR.EOCAL, adc.CALFACT.value) == (0, 1, 0)
    adc.CR.write(ADVREGEN | ADEN)
    settle(m, 2)
    assert adc.ISR.ADRDY == 1
    adc.CR.write(ADVREGEN)
    assert adc.CR.ADEN == 1
    adc.CR.write(ADVREGEN | ADEN | ADDIS)
    settle(m, 1)
    assert (adc.CR.ADEN, adc.CR.ADDIS, adc.ISR.ADRDY) == (0, 0, 1)


def test_adcal_without_the_regulator_raises_under_strict_and_keeps_adc_cr(
    u575: popcorn.PartDescription,
) -> None:
    m = idle(u575, strict=True)
    adc = m.periph.ADC4
    with pytest.raises(ModelError):
        adc.CR.write(ADCAL)
    assert adc.CR.value == 0
    adc.CR.write(ADVREGEN)
    settle(m, 30)
    adc.CR.write(ADVREGEN | ADCAL)
    assert adc.CR.ADCAL == 1

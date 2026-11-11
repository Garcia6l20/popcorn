"""The regulator, calibration and enable sequence of the built-in STM32U5 ADC4 model."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import ModelError, StopReason
from popcorn.units import VirtualDuration, us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
RCC_ELF = PREBUILT / "rcc_ll.elf"
ADEN = 1 << 0
ADDIS = 1 << 1
ADSTART = 1 << 2
ADVREGEN = 1 << 28
ADCAL = 1 << 31
HSION = 1 << 8
ADC4EN = 1 << 5
ADCDACSEL_HSI16 = 4 << 12


def settle(m: popcorn.Machine, duration: int) -> None:
    assert asyncio.run(m.run(for_=us(duration))).reason is StopReason.TIME


def idle(
    u575: popcorn.PartDescription, strict: bool = False, slice_instructions: int = 1_000_000
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        strict_peripherals=strict,
        slice_instructions=slice_instructions,
    )
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSION)
    settle(m, 260)
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


class SequenceEnded:
    def ready(self, machine: popcorn.Machine) -> bool:
        return machine.periph.ADC4.ISR.EOS == 1


def enabled(u575: popcorn.PartDescription, slice_instructions: int = 1_000_000) -> popcorn.Machine:
    m = idle(u575, slice_instructions=slice_instructions)
    adc = m.periph.ADC4
    adc.CR.write(ADVREGEN)
    settle(m, 30)
    adc.CR.write(ADVREGEN | ADEN)
    settle(m, 2)
    assert adc.ISR.ADRDY == 1
    return m


def test_a_driven_pad_converts_against_vdda(u575: popcorn.PartDescription) -> None:
    m = enabled(u575)
    adc = m.periph.ADC4
    adc.input(9).volts = 1.2
    adc.CHSELRMOD0.write(1 << 9)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    settle(m, 2)
    assert (adc.ISR.EOC, adc.ISR.EOS, adc.CR.ADSTART) == (1, 1, 0)
    assert adc.DR.value == int(1.2 * 4096 / 3.0)


def converted(m: popcorn.Machine, running: bool) -> tuple[int, int]:
    adc = m.periph.ADC4
    adc.SMPR.write(7)
    adc.CHSELRMOD0.write(1 << 9)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    driven = m.write((m.ref.periph.ADC4.input(9).volts, 1.5), at=m.time + VirtualDuration(1))
    if running:

        async def body() -> None:
            async with m.running():
                await driven
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 60)
    return adc.DR.value, adc.ISR.EOS


def test_a_drive_during_sampling_converts_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = converted(enabled(u575), running=False)
    assert parked == (int(1.5 * 4096 / 3.0), 1)
    assert converted(enabled(u575, slice_instructions=1_000), running=True) == parked


TIM15EN = 1 << 16
EXTEN_RISING = 1 << 10
EXTSEL_TIM15_TRGO = 3 << 6
OVRMOD = 1 << 12


def triggered_scan(m: popcorn.Machine, running: bool) -> tuple[int, int, int]:
    rcc = m.periph.RCC
    adc = m.periph.ADC4
    rcc.RCC_APB2ENR.write(rcc.RCC_APB2ENR.value | TIM15EN)
    tim = m.periph.TIM15
    tim.PSC.write(0)
    tim.ARR.write(3999)
    tim.EGR.write(1)
    tim.SR.write(0)
    tim.CR2.write(2 << 4)
    tim.CR1.write(1)
    adc.input(9).volts = 1.5
    adc.CHSELRMOD0.write(1 << 9)
    adc.CFGR1.write(OVRMOD | EXTEN_RISING | EXTSEL_TIM15_TRGO)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    if running:

        async def body() -> None:
            async with m.running():
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 1_200)
    return adc.DR.value, adc.ISR.EOS, adc.CR.ADSTART


def test_a_triggered_scan_converts_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = triggered_scan(enabled(u575), running=False)
    assert parked == (int(1.5 * 4096 / 3.0), 1, 1)
    assert triggered_scan(enabled(u575, slice_instructions=1_000), running=True) == parked


AWD1EN = 1 << 23


def watched_scan(m: popcorn.Machine, running: bool) -> tuple[int, int]:
    adc = m.periph.ADC4
    adc.SMPR.write(7)
    adc.CFGR1.write(AWD1EN)
    adc.AWD1TR.write((0x800 << 16) | 0x400)
    adc.CHSELRMOD0.write(1 << 9)
    adc.input(9).volts = 1.0
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    driven = m.write((m.ref.periph.ADC4.input(9).volts, 2.0), at=m.time + VirtualDuration(1))
    if running:

        async def body() -> None:
            async with m.running():
                await driven
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 60)
    return adc.DR.value, adc.ISR.AWD1


def test_a_drive_out_of_the_window_sets_awd1_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = watched_scan(enabled(u575), running=False)
    assert parked == (int(2.0 * 4096 / 3.0), 1)
    assert watched_scan(enabled(u575, slice_instructions=1_000), running=True) == parked


def oversampled_scan(m: popcorn.Machine, running: bool) -> tuple[int, int]:
    adc = m.periph.ADC4
    adc.SMPR.write(7)
    adc.CFGR2.write(1)
    adc.CHSELRMOD0.write(1 << 9)
    adc.input(9).volts = 1.0
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    driven = m.write((m.ref.periph.ADC4.input(9).volts, 2.0), at=m.time + VirtualDuration(1))
    if running:

        async def body() -> None:
            async with m.running():
                await driven
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 260)
    return adc.DR.value, adc.ISR.EOS


def test_an_oversampled_scan_converts_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = oversampled_scan(enabled(u575), running=False)
    assert parked == (2 * int(2.0 * 4096 / 3.0), 1)
    assert oversampled_scan(enabled(u575, slice_instructions=1_000), running=True) == parked


DAC1EN = 1 << 6
DAC_MODE_CHIP = 1
DAC_EN1 = 1
CHN21 = 1 << 21


def dac_channel(m: popcorn.Machine, running: bool) -> tuple[int, int]:
    rcc = m.periph.RCC
    rcc.RCC_AHB3ENR.write(rcc.RCC_AHB3ENR.value | DAC1EN)
    dac = m.periph.DAC1
    dac.MCR.write(DAC_MODE_CHIP)
    dac.CR.write(DAC_EN1)
    settle(m, 10)
    dac.DHR12R1.write(0x800)
    settle(m, 2)
    adc = m.periph.ADC4
    adc.CHSELRMOD0.write(CHN21)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    if running:

        async def body() -> None:
            async with m.running():
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 2)
    return adc.DR.value, adc.ISR.EOS


def test_a_dac1_output_converts_on_channel_21_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = dac_channel(enabled(u575), running=False)
    assert parked == (0x800, 1)
    assert dac_channel(enabled(u575, slice_instructions=1_000), running=True) == parked


WAIT = 1 << 14
EOC = 1 << 2


class ConversionDone:
    def ready(self, machine: popcorn.Machine) -> bool:
        return machine.periph.ADC4.ISR.EOC == 1


def waited(m: popcorn.Machine, running: bool) -> tuple[int, int, int, int]:
    adc = m.periph.ADC4
    adc.input(9).volts = 1.0
    adc.input(10).volts = 2.0
    adc.CHSELRMOD0.write((1 << 9) | (1 << 10))
    adc.CFGR1.write((1 << 13) | WAIT)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    if running:

        async def body() -> None:
            async with m.running():
                await m.wait_for(ConversionDone())

        asyncio.run(body())
    else:
        settle(m, 30)
    settle(m, 30)
    first = adc.DR.value
    held = adc.ISR.OVR
    adc.ISR.write(EOC)
    settle(m, 30)
    return first, held, adc.DR.value, adc.ISR.EOS


def test_a_wait_mode_conversion_holds_until_eoc_clears_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = waited(enabled(u575), running=False)
    assert parked == (int(1.0 * 4096 / 3.0), 0, int(2.0 * 4096 / 3.0), 1)
    assert waited(enabled(u575, slice_instructions=1_000), running=True) == parked


LPTIM1EN = 1 << 11
EXTSEL_LPTIM1_CH1 = 5 << 6
LPTIM1SEL_HSI16 = 2 << 10


def lptim_scan(m: popcorn.Machine, running: bool) -> tuple[int, int, int]:
    adc = m.periph.ADC4
    adc.input(9).volts = 1.5
    adc.CHSELRMOD0.write(1 << 9)
    adc.CFGR1.write(OVRMOD | EXTEN_RISING | EXTSEL_LPTIM1_CH1)
    adc.CR.write(ADVREGEN | ADEN | ADSTART)
    rcc = m.periph.RCC
    rcc.RCC_APB3ENR.write(rcc.RCC_APB3ENR.value | LPTIM1EN)
    rcc.RCC_CCIPR3.write(rcc.RCC_CCIPR3.value | LPTIM1SEL_HSI16)
    lptim = m.periph.LPTIM1
    lptim.CCMR1.write(1 << 1)
    lptim.CFGR.write(0)
    lptim.CR.write(1)
    lptim.DIER_output.write(0)
    settle(m, 2)
    lptim.ICR_output.write(1 << 24)
    lptim.ARR.write(200)
    settle(m, 2)
    lptim.CCR1.write(2)
    settle(m, 2)
    lptim.ICR_output.write((1 << 3) | (1 << 4))
    lptim.CR.write(1 | (1 << 2))
    if running:

        async def body() -> None:
            async with m.running():
                await m.wait_for(SequenceEnded())

        asyncio.run(body())
    else:
        settle(m, 10)
    return adc.DR.value, adc.ISR.EOS, adc.CR.ADSTART


def test_an_lptim_output_starts_a_scan_the_same_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = lptim_scan(enabled(u575), running=False)
    assert parked == (int(1.5 * 4096 / 3.0), 1, 1)
    assert lptim_scan(enabled(u575, slice_instructions=1_000), running=True) == parked

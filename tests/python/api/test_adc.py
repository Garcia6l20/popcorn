"""External ADC inputs, the board supply and the ADC clock range warning of the built-in ADC1."""

from __future__ import annotations

import asyncio
import logging
import threading
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, StopReason
from popcorn.board import AnalogInput, SupplyOptions
from popcorn.units import VirtualDuration, us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
RCC_ELF = PREBUILT / "rcc_ll.elf"
ADEN = 1 << 0
ADDIS = 1 << 1
ADSTART = 1 << 2
ADVREGEN = 1 << 28
HSION = 1 << 8
PLL2ON = 1 << 26
ADC12EN = 1 << 10
ADCDACSEL_PLL2R = 2 << 12
ADCDACSEL_HSI16 = 4 << 12
ALL_CHANNELS = 0xF_FFFF


def settle(m: popcorn.Machine, duration: int = 200) -> None:
    assert asyncio.run(m.run(for_=us(duration))).reason is StopReason.TIME


def idle(
    u575: popcorn.PartDescription,
    supply: SupplyOptions | None = None,
    slice_instructions: int = 1_000_000,
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(RCC_ELF),
        supply=supply,
        slice_instructions=slice_instructions,
    )
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSION)
    settle(m)
    assert rcc.RCC_CR.HSIRDY == 1
    rcc.RCC_AHB2ENR1.write(rcc.RCC_AHB2ENR1.value | ADC12EN)
    return m


def at_16_mhz(m: popcorn.Machine) -> None:
    m.periph.RCC.RCC_CCIPR3.write(ADCDACSEL_HSI16)


def at_72_mhz(m: popcorn.Machine) -> None:
    rcc = m.periph.RCC
    rcc.RCC_PLL2DIVR.write(8 | (1 << 9) | (1 << 16) | (1 << 24))
    rcc.RCC_PLL2CFGR.write(2 | (3 << 2) | (1 << 18))
    rcc.RCC_CR.write(rcc.RCC_CR.value | PLL2ON)
    settle(m)
    assert rcc.RCC_CR.PLL2RDY == 1
    rcc.RCC_CCIPR3.write(ADCDACSEL_PLL2R)


def enable(m: popcorn.Machine) -> None:
    m.periph.ADC1.ADC_PCSEL.write(ALL_CHANNELS)
    m.periph.ADC1.ADC_CR.write(ADVREGEN)
    m.periph.ADC1.ADC_CR.write(ADVREGEN | ADEN)


def convert(m: popcorn.Machine, channel: int) -> int:
    adc = m.periph.ADC1
    adc.ADC_SQR1.write(channel << 6)
    adc.ADC_CR.write(ADVREGEN | ADEN | ADSTART)
    assert adc.ADC_ISR.EOC == 0
    settle(m, 60)
    assert adc.ADC_ISR.EOC == 1
    return adc.ADC_DR.value


def ready(u575: popcorn.PartDescription, supply: SupplyOptions | None = None) -> popcorn.Machine:
    m = idle(u575, supply)
    at_16_mhz(m)
    enable(m)
    return m


def test_a_driven_channel_converts_at_the_default_vdda(u575: popcorn.PartDescription) -> None:
    m = ready(u575)
    channel = m.periph.ADC1.input(5)
    assert isinstance(channel, AnalogInput)
    assert channel.volts == 0.0
    channel.volts = 1.2
    assert channel.volts == 1.2
    assert convert(m, 5) == int(1.2 * 16384 / 3.0)


def test_the_supply_sets_vdda_from_the_machine_or_the_project(
    u575: popcorn.PartDescription,
) -> None:
    assert SupplyOptions() == SupplyOptions(vdda_mv=3000)
    m = ready(u575, SupplyOptions(vdda_mv=3300))
    m.periph.ADC1.input(5).volts = 1.65
    assert convert(m, 5) == 8192
    project = popcorn.Project(
        part="STM32U575QGIx", images=[popcorn.Elf(RCC_ELF)], supply=SupplyOptions(vdda_mv=3300)
    )
    m = project.machine()
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is StopReason.ADDRESS
    m.reg.pc = m.sym("Default_Handler")
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSION)
    settle(m)
    rcc.RCC_AHB2ENR1.write(rcc.RCC_AHB2ENR1.value | ADC12EN)
    at_16_mhz(m)
    enable(m)
    m.periph.ADC1.input(5).volts = 1.65
    assert convert(m, 5) == 8192


@pytest.mark.parametrize("vdda_mv", [1000, 1619, 3601, 3700])
def test_a_vdda_outside_the_datasheet_range_is_refused(
    u575: popcorn.PartDescription, vdda_mv: int
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(u575, supply=SupplyOptions(vdda_mv=vdda_mv))
    assert caught.value.key == "supply.vdda_mv"
    project = popcorn.Project(
        part="STM32U575QGIx", images=[], supply=SupplyOptions(vdda_mv=vdda_mv)
    )
    with pytest.raises(ConfigError) as caught:
        project.machine()
    assert caught.value.key == "supply.vdda_mv"


def test_only_external_channels_take_an_input(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575)
    for channel in (0, 18, 19, 20):
        with pytest.raises(ConfigError):
            m.periph.ADC1.input(channel)
    with pytest.raises(popcorn.ModelError):
        _ = m.periph.USART1.input(5).volts
    for volts in (-0.1, float("nan"), float("inf"), 4295.0):
        with pytest.raises(ValueError, match="V is not a voltage"):
            m.periph.ADC1.input(5).volts = volts


def test_the_input_is_board_state_in_snapshots_and_across_a_reset(
    u575: popcorn.PartDescription,
) -> None:
    m = ready(u575)
    channel = m.periph.ADC1.input(3)
    channel.volts = 1.0
    saved = m.snapshot()
    channel.volts = 2.0
    m.restore(saved)
    assert channel.volts == 1.0
    assert convert(m, 3) == int(1.0 * 16384 / 3.0)
    m.reset()
    assert channel.volts == 1.0


def test_a_drive_from_another_thread_during_a_run_raises(
    u575: popcorn.PartDescription,
) -> None:
    m = ready(u575)
    errors: list[BaseException] = []

    def drive() -> None:
        try:
            m.periph.ADC1.input(5).volts = 1.0
        except BaseException as error:
            errors.append(error)

    async def body() -> None:
        async with m.running():
            thread = threading.Thread(target=drive)
            thread.start()
            while thread.is_alive():
                await asyncio.sleep(0.001)

    asyncio.run(body())
    assert len(errors) == 1 and isinstance(errors[0], popcorn.MachineRunning)


class ConversionEnded:
    def ready(self, machine: popcorn.Machine) -> bool:
        return machine.periph.ADC1.ADC_ISR.EOC == 1


def test_a_drive_inside_running_reaches_the_conversion(u575: popcorn.PartDescription) -> None:
    m = idle(u575, slice_instructions=1_000)
    at_16_mhz(m)
    enable(m)
    adc = m.periph.ADC1
    adc.ADC_SMPR1.write(7 << 15)
    m.periph.ADC12.ADC12_CCR.write(11 << 18)
    adc.ADC_SQR1.write(5 << 6)
    adc.ADC_CR.write(ADVREGEN | ADEN | ADSTART)

    driven = m.write((m.ref.periph.ADC1.input(5).volts, 1.5), at=m.time + VirtualDuration(1))

    async def body() -> None:
        async with m.running():
            await driven
            await m.wait_for(ConversionEnded())

    asyncio.run(body())
    assert adc.ADC_ISR.EOC == 1
    assert adc.ADC_DR.value == int(1.5 * 16384 / 3.0)


def range_warnings(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [
        r
        for r in caplog.records
        if getattr(r, "popcorn_peripheral", None) == "ADC1" and "after PRESC" in r.getMessage()
    ]


def test_an_adc_clock_above_55_mhz_warns_on_each_enable(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    m = idle(u575)
    at_72_mhz(m)
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        enable(m)
        m.periph.ADC1.ADC_CR.write(ADVREGEN | ADDIS)
        enable(m)
    warned = range_warnings(caplog)
    assert len(warned) == 2
    assert all("72000000 Hz" in r.getMessage() for r in warned)


def test_an_adc_clock_at_16_mhz_does_not_warn(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        ready(u575)
    assert range_warnings(caplog) == []

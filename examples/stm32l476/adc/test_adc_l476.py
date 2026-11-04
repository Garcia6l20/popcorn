"""The STM32L476 ADC1 measures its internal reference, its temperature sensor and PA0, HAL.

The firmware switches the core to HSI16 in voltage range 1 and selects SYSCLK as the ADC kernel
clock in `RCC_CCIPR.ADCSEL`. `HAL_ADC_Init` leaves deep power-down and starts the ADC voltage
regulator, `HAL_ADCEx_Calibration_Start` runs the single-ended calibration. ADC1 then converts
VREFINT on channel 0 and the temperature sensor on channel 17 once each at 12 bits, 640.5 cycles
of sampling time, and turns them into VDDA in mV and a temperature in degrees C from the factory
words: VREFINT_CAL at 0x1FFF75AA, TS_CAL1 at 0x1FFF75A8 and TS_CAL2 at 0x1FFF75CA. Then it
converts PA0, ADC1_IN5, once per millisecond into `pa0_data` and `pa0_mv`.

It ports `stm32l152/adc`. PA0 takes `GPIO_MODE_ANALOG_ADC_CONTROL`, which also closes the
`GPIOA_ASCR.ASC0` switch between the pad and the ADC, RM0351 18.4.11. The model reads channels 0
and 17 as the factory words scaled by 3000 / VDDA, VDDA from the description's `SupplyOptions`,
3.0 V by default. `m.periph.ADC1.input(5).volts` and `m.periph.GPIOA.pin(0).volts` both drive the
pad, read as V x 4096 / VDDA, floored and saturated at 4095. The project sets
`MachineDescription(strict=True)`.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=2_000_000)

VREFINT_CAL_ADDRESS = 0x1FFF75AA
TS_CAL1_ADDRESS = 0x1FFF75A8
ANALOG_MODE = 3
SYSCLK_ADC_CLOCK = 3
FULL_SCALE = 4095


def factory_word(machine: Machine, address: int) -> int:
    return int.from_bytes(machine.mem.read(address, 2), "little")


async def measure(machine: Machine) -> None:
    stop = await machine.run(until="measured", for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop


async def next_pa0(machine: Machine) -> int:
    stop = await machine.run(until="pa0_converted", for_=ms(5))
    assert stop.reason is StopReason.ADDRESS, stop
    value = machine.var("pa0_data").value
    assert isinstance(value, int)
    return value


async def test_vdda_and_temperature_at_reset_conditions(adc: Machine) -> None:
    await measure(adc)
    assert adc.var("vrefint_data").value == factory_word(adc, VREFINT_CAL_ADDRESS)
    assert adc.var("vdda_mv").value == 3000
    assert adc.var("sensor_data").value == factory_word(adc, TS_CAL1_ADDRESS)
    assert adc.var("temperature_c").value == 30


async def test_the_internal_channels_scale_with_a_3v3_vdda(adc_3v3: Machine) -> None:
    await measure(adc_3v3)
    vrefint_cal = factory_word(adc_3v3, VREFINT_CAL_ADDRESS)
    vrefint_data = vrefint_cal * 3000 // 3300
    assert adc_3v3.var("vrefint_data").value == vrefint_data
    assert adc_3v3.var("vdda_mv").value == 3000 * vrefint_cal // vrefint_data == 3301
    assert adc_3v3.var("sensor_data").value == factory_word(adc_3v3, TS_CAL1_ADDRESS) * 3000 // 3300
    assert adc_3v3.var("temperature_c").value == 30


async def test_the_msp_init_clocks_the_adc_from_sysclk_and_closes_the_pa0_switch(
    adc: Machine,
) -> None:
    assert adc.periph.RCC.AHB2ENR.ADCEN == 0
    assert adc.periph.GPIOA.ASCR.ASC0 == 0
    await measure(adc)
    assert adc.periph.RCC.AHB2ENR.ADCEN == 1
    assert adc.periph.RCC.CCIPR.ADCSEL == SYSCLK_ADC_CLOCK
    assert adc.periph.GPIOA.MODER.MODER0 == ANALOG_MODE
    assert adc.periph.GPIOA.ASCR.ASC0 == 1
    assert adc.periph.ADC1.CR.DEEPPWD == 0
    assert adc.periph.ADC1.CR.ADVREGEN == 1
    assert adc.periph.ADC_Common.CCR.VREFEN == 1
    assert adc.periph.ADC_Common.CCR.CH17SEL == 1


@pytest.mark.parametrize("volts", [0.0, 1.25, 2.9995, 3.2])
async def test_pa0_reads_its_driven_voltage(adc: Machine, volts: float) -> None:
    await measure(adc)
    await next_pa0(adc)
    adc.periph.ADC1.input(5).volts = volts
    assert await next_pa0(adc) == min(int(volts * 4096 / 3.0), FULL_SCALE)


async def test_a_pa0_drive_through_the_gpio_pin_converts_on_adc1_in5(adc: Machine) -> None:
    await measure(adc)
    await next_pa0(adc)
    adc.periph.GPIOA.pin(0).volts = 1.5
    assert adc.periph.ADC1.input(5).volts == pytest.approx(1.5)
    assert await next_pa0(adc) == 2048
    assert adc.var("pa0_mv").value == 2048 * 3000 // FULL_SCALE


async def test_pa0_codes_at_a_3v3_vdda(adc_3v3: Machine) -> None:
    await measure(adc_3v3)
    await next_pa0(adc_3v3)
    adc_3v3.periph.ADC1.input(5).volts = 1.25
    assert await next_pa0(adc_3v3) == 1551
    assert adc_3v3.var("pa0_mv").value == 1551 * 3301 // FULL_SCALE

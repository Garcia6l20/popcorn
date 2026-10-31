"""The STM32L1 ADC measures its internal reference, its temperature sensor and PA0, HAL.

The firmware switches the core to HSI16, which also clocks the ADC, then converts VREFINT on
channel 17 and the temperature sensor on channel 16 once each at 12 bits, 384 cycles of sampling
time, with `HAL_ADC_ConfigChannel`, `HAL_ADC_Start` and `HAL_ADC_PollForConversion`. It turns
them into VDDA in mV and a temperature in degrees C from the factory words: VREFINT_CAL at
0x1FF800F8, TS_CAL1 at 0x1FF800FA and TS_CAL2 at 0x1FF800FE. Then it converts PA0, ADC_IN0, once
per millisecond into `pa0_data` and `pa0_mv`.

The model reads channels 16 and 17 as the factory words scaled by 3000 / VDDA, VDDA from the
description's `SupplyOptions`, 3.0 V by default. `m.periph.ADC.input(0).volts` drives PA0, read
as V x 4096 / VDDA, floored and saturated at 4095. The project sets
`MachineDescription(strict=True)`.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=2_000_000)

VREFINT_CAL_ADDRESS = 0x1FF800F8
TS_CAL1_ADDRESS = 0x1FF800FA
ANALOG_MODE = 3
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
    assert adc_3v3.var("vrefint_data").value == vrefint_cal * 3000 // 3300
    assert adc_3v3.var("vdda_mv").value == 3300
    assert adc_3v3.var("sensor_data").value == factory_word(adc_3v3, TS_CAL1_ADDRESS) * 3000 // 3300
    assert adc_3v3.var("temperature_c").value == 30


async def test_the_msp_init_enables_the_adc_and_sets_pa0_analog(adc: Machine) -> None:
    assert adc.periph.RCC.APB2ENR.ADC1EN == 0
    await measure(adc)
    assert adc.periph.RCC.APB2ENR.ADC1EN == 1
    assert adc.periph.GPIOA.MODER.MODER0 == ANALOG_MODE
    assert adc.periph.ADC.CCR.TSVREFE == 1
    assert adc.periph.RCC.CR.HSIRDY == 1


@pytest.mark.parametrize("volts", [0.0, 1.25, 2.9995, 3.2])
async def test_pa0_reads_its_driven_voltage(adc: Machine, volts: float) -> None:
    await measure(adc)
    await next_pa0(adc)
    adc.periph.ADC.input(0).volts = volts
    assert await next_pa0(adc) == min(int(volts * 4096 / 3.0), FULL_SCALE)


async def test_pa0_codes_at_a_3v3_vdda(adc_3v3: Machine) -> None:
    await measure(adc_3v3)
    await next_pa0(adc_3v3)
    adc_3v3.periph.ADC.input(0).volts = 1.25
    assert await next_pa0(adc_3v3) == 1551
    assert adc_3v3.var("pa0_mv").value == 1551 * 3300 // FULL_SCALE

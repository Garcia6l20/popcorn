"""ADC1 measures its internal reference and temperature sensor through the LL driver.

The firmware leaves deep power-down, enables the ADC voltage regulator and waits for LDORDY,
runs an offset calibration until ADCAL clears, enables ADC1 until ADRDY, then converts VREFINT
and the temperature sensor once each at 14 bits. The LL macros turn the results into VDDA in mV
and a temperature in degrees C from the factory calibration words in the signature page:
VREFINT_CAL at 0x0BFA07A5, TS_CAL1 at 0x0BFA0710 and TS_CAL2 at 0x0BFA0742. At reset conditions
they give 3.0 V and 30 C. A last VREFINT conversion at 12 bits drops the two low bits.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=2_000_000)

VREFINT_CAL = 6635
TS_CAL1 = 4107


async def test_vdda_and_temperature_at_reset_conditions(adc: Machine) -> None:
    stop = await adc.run(until="measured", for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop
    assert adc.var("vrefint_data").value == VREFINT_CAL
    assert adc.var("vref_mv").value == 3000
    assert adc.var("sensor_data").value == TS_CAL1
    assert adc.var("temperature_c").value == 30
    assert adc.var("vrefint_data_12b").value == VREFINT_CAL >> 2

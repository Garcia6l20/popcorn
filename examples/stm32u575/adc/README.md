# ADC1 internal channels, LL

An STM32U575 application powers up ADC1 with the LL driver: VDDA valid, HCLK as the ADC kernel
clock, VREFINT and the temperature sensor connected in ADC12_CCR, deep power-down left, the
voltage regulator enabled until LDORDY, an offset calibration until ADCAL clears and ADC1
enabled until ADRDY. It converts VREFINT and the temperature sensor once each at 14 bits, turns
them into VDDA in mV and a temperature in degrees C with `__LL_ADC_CALC_VREFANALOG_VOLTAGE` and
`__LL_ADC_CALC_TEMPERATURE`, then converts VREFINT again at 12 bits, and reaches `measured`.

popcorn's ADC1 model completes the regulator start-up, the calibration and the enable at the
write. A conversion takes its real time, (SMP + TSAR) ADC clock cycles: 814 + 17 cycles at the
4 MHz HCLK, about 208 us, and the firmware polls EOC through it. The model converts at the
board's VDDA, 3.0 V by default, and 30 C: VREFINT reads the factory VREFINT_CAL word and the
temperature sensor TS_CAL1, both stored in the signature page at datasheet typical values, so
the LL macros give 3000 mV and 30 C. External channels read the voltage driven on them, such as
`m.periph.ADC1.input(5).volts = 1.2`, and 0 when never driven.

The 4 MHz ADC clock is below the 5 MHz datasheet minimum, so each of the two ADC enables logs
`ADC clock 4000000 Hz after PRESC is outside 5 to 55 MHz`. The conversions keep their computed
time and the test does not read the log.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/adc
```

## Read first

- `test_adc.py`: runs to `measured` and checks the raw codes and the computed VDDA and
  temperature.
- `firmware/main.c`: the power-up, calibration and conversion sequence.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/adc/adc_internal.elf examples/stm32u575/adc/prebuilt/
```

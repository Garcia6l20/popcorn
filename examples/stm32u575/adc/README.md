# ADC1 internal channels, LL

An STM32U575 application powers up ADC1 with the LL driver: VDDA valid, HCLK as the ADC kernel
clock, VREFINT and the temperature sensor connected in ADC12_CCR, deep power-down left, the
voltage regulator enabled until LDORDY, an offset calibration until ADCAL clears and ADC1
enabled until ADRDY. It converts VREFINT and the temperature sensor once each at 14 bits, turns
them into VDDA in mV and a temperature in degrees C with `__LL_ADC_CALC_VREFANALOG_VOLTAGE` and
`__LL_ADC_CALC_TEMPERATURE`, then converts VREFINT again at 12 bits, and reaches `measured`.

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

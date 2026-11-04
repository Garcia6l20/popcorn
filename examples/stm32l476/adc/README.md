# ADC internal channels and an external input, HAL

An STM32L476 application switches the core to the 16 MHz HSI in voltage range 1, selects SYSCLK
as the ADC kernel clock in RCC_CCIPR.ADCSEL and converts with ST's HAL ADC driver at 12 bits and
640.5 cycles of sampling time. `HAL_ADC_Init` leaves deep power-down and starts the ADC voltage
regulator, `HAL_ADCEx_Calibration_Start` calibrates the single-ended path. ADC1 converts VREFINT
on channel 0 and the temperature sensor on channel 17 once each, and turns them into VDDA in mV
and a temperature in degrees C from the factory words VREFINT_CAL, TS_CAL1 and TS_CAL2 at
0x1FFF75AA, 0x1FFF75A8 and 0x1FFF75CA. Then it converts PA0, ADC1_IN5, once per millisecond
into `pa0_data` and `pa0_mv`.

It ports `stm32l152/adc`. PA0 takes `GPIO_MODE_ANALOG_ADC_CONTROL`, which closes its
GPIOA_ASCR.ASC0 switch to the ADC, RM0351 18.4.11. The test drives PA0 with
`m.periph.ADC1.input(5).volts` and with the pin's own `m.periph.GPIOA.pin(0).volts`, one level
for both, and the `vdda_3v3` config sets `SupplyOptions(vdda_mv=3300)`.
`MachineDescription(strict=True)` makes any register access the models do not serve fail the
run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/adc
```

## Read first

- `test_adc_l476.py`:
  - at the default 3.0 V the internal channels read the factory words, 3000 mV and 30 C;
  - at 3.3 V they read the factory words scaled by 3000 / 3300, and the firmware finds 3301 mV,
    the integer division of its VREFINT arithmetic, and 30 C;
  - ADCEN, ADCSEL at SYSCLK, PA0 in analog mode with ASC0 set, DEEPPWD, ADVREGEN, VREFEN and
    CH17SEL;
  - PA0 driven to a voltage, the next conversion's code floored and saturated at 4095;
  - PA0 driven through the GPIO pin view, read by ADC1 channel 5.
- `conftest.py`: the description and its `vdda_3v3` config, two fixtures.
- `firmware/main.c`: the clock setup, the calibration, one conversion per call, and the
  calibration word arithmetic.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/adc/adc_internal.elf examples/stm32l476/adc/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.

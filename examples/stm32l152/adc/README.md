# ADC internal channels and an external input, HAL

An STM32L152 application switches the core to the 16 MHz HSI, the clock of the STM32L1 ADC, and
converts with ST's HAL ADC driver at 12 bits and 384 cycles of sampling time. It converts
VREFINT on channel 17 and the temperature sensor on channel 16 once each, and turns them into
VDDA in mV and a temperature in degrees C from the factory words VREFINT_CAL, TS_CAL1 and
TS_CAL2 at 0x1FF800F8, 0x1FF800FA and 0x1FF800FE. Then it converts PA0, ADC_IN0, once per
millisecond into `pa0_data` and `pa0_mv`.

It ports `stm32u575/adc` to the STM32L1 ADC, with an external channel added. The test drives PA0
with `m.periph.ADC.input(0).volts`, and the `vdda_3v3` config sets
`SupplyOptions(vdda_mv=3300)`. `MachineDescription(strict=True)` makes any register access the
models do not serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/adc
```

## Read first

- `test_adc_l152.py`:
  - at the default 3.0 V the internal channels read the factory words, 3000 mV and 30 C;
  - at 3.3 V they read the factory words scaled by 3000 / 3300, and the firmware still finds
    3300 mV and 30 C;
  - ADC1EN, PA0 in analog mode, TSVREFE and HSIRDY;
  - PA0 driven to a voltage, the next conversion's code floored and saturated at 4095.
- `conftest.py`: the description and its `vdda_3v3` config, two fixtures.
- `firmware/main.c`: the clock setup, one conversion per call, and the calibration arithmetic.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/adc/adc_internal.elf examples/stm32l152/adc/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.

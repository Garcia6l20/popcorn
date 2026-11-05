# DAC triangle read back by the ADC on chip, HAL

An STM32L476 application switches the core to the 16 MHz HSI in voltage range 1 with no wait
state and sets DAC1 channel 1 up with ST's HAL DAC driver: the TIM6 TRGO trigger, the output
buffer on and the on-chip connection, MODE1 001, which drives both PA4 and the ADC2 channel 17
path, RM0351 Table 130. `HAL_DACEx_TriangleWaveGenerate` adds a triangle of amplitude 7 to the
0x400 in DAC_DHR12R1, RM0351 19.4.9. TIM6 counts at 1 MHz and its update, the TRGO with
CR2.MMS 010, loads the next step into DAC_DOR1 at 1 kHz. After each update ADC2 converts
channel 17, `ADC_CHANNEL_DAC1CH1_ADC2`, into `samples`, and the firmware calls `sampled` after
16 updates.

`HAL_TIM_Base_Init` sets TIM6_EGR.UG while CR2.MMS is still 000, and MMS 000 sends UG as the
TRGO, RM0351 33.4.2. `HAL_TIMEx_MasterConfigSynchronization` then selects the update, MMS 010.
It reads offset 0x08 too, TIMx_SMCR on other timers and reserved on TIM6, which the model reads
as 0. So the DAC loads 0x400 at init and the first update loads 0x401, inferred: the model adds
the triangle counter, then steps it, after RM0351 19.4.12 and Figure 144, where 19.4.9 steps
before the add. `MachineDescription(strict=True)` makes any register access the models do not
serve fail the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/dac
```

## Read first

- `test_dac_l476.py`:
  - ADC2 reads the 16 triangle steps back code for code, at the default 3.0 V and at 3.3 V
    VDDA;
  - DAC1EN, MODE1, EN1, TEN1, TSEL1, WAVE1, MAMP1, TIM6 MMS and PA4 in analog mode;
  - DAC_DOR1 at 0x400 once TIM6 is set up, from the UG trigger at init, the value inferred;
  - `m.periph.GPIOA.pin(4).volts` at DAC_DOR1 x VDDA / 4096;
  - a board drive of PA4 yielding to the DAC output with one warning.
- `conftest.py`: the description and its `vdda_3v3` config, two fixtures.
- `firmware/main.c`: the clock setup, the DAC, ADC and TIM6 setup, one conversion per update.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/dac/dac_triangle.elf examples/stm32l476/dac/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.

# A TIM15-triggered ADC1 scan with GPDMA and an analog watchdog, HAL

An STM32U575 application scans four external ADC1 inputs on each TIM15 update and watches one of
them with AWD3:

- TIM15 counts at 1 kHz from the 4 MHz MSIS clock and updates every 10 ms. Its TRGO, MMS 010,
  is the ADC1 regular trigger, `ADC_EXTERNALTRIG_T15_TRGO` on the rising edge.
- ADC1 runs from HSI16 and converts IN1, IN2, IN5 and IN6 at 12 bits, 68 cycles of sampling,
  8x oversampling shifted right by 3.
- GPDMA1 channel 0 moves each result to `samples` from a one-node circular list, two scans
  deep. The half and complete callbacks copy the scan that just landed into `monitor.latest`.
- AWD3 watches IN6 with a window of 1241 to 2482, 1.0 V to 2.0 V at 3.3 V. Each result out of
  the window calls `HAL_ADCEx_LevelOutOfWindow3Callback`, which counts `monitor.alarms`.

The `Project` in `conftest.py` sets the board supply that ADC1 converts against, and makes any
register access popcorn does not model fail the run:

```python
supply=SupplyOptions(vdda_mv=3300),
strict=True,
```

A test building its own machine passes `Machine(supply=SupplyOptions(vdda_mv=3300))`, with
`SupplyOptions` from `popcorn.board`. Without either, VDDA is 3.0 V.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/adc_watchdog
```

## Read first

- `test_adc_watchdog.py`: drives the four inputs and checks:
  - each scan's four codes in `samples` and `monitor.latest`, 3 scans in 35 ms.
  - one alarm per scan while IN6 is above or below the window, none inside it or for another
    input driven out of the window mid-run.
  - the trip points one microvolt apart on both thresholds.
  - the scan end 166.1875 us after each TIM15 update, the scans 10 ms apart.
  - a snapshot keeps the driven inputs.
- `firmware/main.c`: the ADC, watchdog, DMA queue and TIM15 setup, and the callbacks.

## Driving the inputs

```python
machine.periph.ADC1.input(6).volts = 2.5
```

`input(n)` is ADC1_INn, 1 to 17. An input is board state: it survives a reset and a snapshot
restores it. A conversion holds the voltage at the end of its own sampling window, so a change
counts from the next sample on. A change alone raises nothing: the watchdog compares
conversion results.

The code of a voltage V is `V x 16384 / VDDA` truncated to 14 bits, then shifted to the
configured resolution. The 8 samples of a rank give the same code, so the oversampled mean is
that code too:

| volts | IN | code |
| --- | --- | --- |
| 0.5 | 1 | 620 |
| 1.0 | 2 | 1241 |
| 2.5 | 5 | 3103 |
| 1.5 | 6 | 1861 |

## Timing

A rank is 8 x (68 + 15) cycles of the 16 MHz ADC clock, 41.5 us. The scan starts 3 ADC cycles,
187.5 ns, after the TRGO edge. It ends 4 x 41.5 us + 187.5 ns = 166.1875 us after the TIM15
update, when EOS and the last rank's AWD3 flag rise together.

## The watchdog comparison

AWD3 flags a result strictly outside its thresholds, RM0456 section 33.4.30: a result above the
high threshold or below the low one. A result equal to a threshold is inside the window. HAL
writes the 12-bit thresholds shifted to the 14-bit scale the comparison uses.

| IN6 volts | code | AWD3 |
| --- | --- | --- |
| 2.000463 | 2482 | no |
| 2.000464 | 2483 | yes |
| 0.999830 | 1241 | no |
| 0.999829 | 1240 | yes |

The flag rises at the end of the conversion of a guarded channel, here the last rank, before
the DMA has moved that result.

## Two HAL details

- For ADC1, HAL writes `Init.Oversampling.Ratio` into ADC_CFGR2.OSR as is, and OSR is the
  ratio minus 1, RM0456 section 33.6. `stm32u5xx_hal_adc.h` documents `Ratio` for ADC1 as "in
  the range from 0 to 1023". `ADC_OVERSAMPLING_RATIO_8` is the ADC4 encoding. On ADC1 it selects
  9 samples, so with the right shift of 3 every code reads 9/8 too high, 698 instead of 620 at
  0.5 V. The firmware writes 7.
- `HAL_ADCEx_Calibration_Start` reads DBGMCU_IDCODE to pick a calibration sequence. popcorn
  returns 0x30076482, DEV_ID 0x482 and REV_ID 0x3007, so HAL runs the sequence for revision U
  and later.

## Model limits

- Conversions never overlap: each rank starts when the one before ends.
- One-shot DMA, DMNGT 01, re-arms only on a CPU write taking ADSTART from 0 to 1, never on a
  trigger. Results after the last DMA transfer still land in ADC_DR.
- AWDFILT1 with DMA does not gate the DMA requests by the window. It logs once and is a model
  error with strict peripherals.
- An ADC clock outside 5 to 55 MHz keeps its computed timing and logs a warning at each enable.
- The trigger latency for ADC12_CCR.PRESC 3 and up is 3 x D + 1 ADC cycles for a divider D, a
  value the datasheet does not give.
- TRGO under MMS 000, 010 and 011 is a pulse one timer kernel clock wide.
- ADC4 has no model.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/adc_watchdog/adc_watchdog.elf examples/stm32u575/adc_watchdog/prebuilt/
```

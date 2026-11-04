# Flash wait states, page erase and program, option bytes, HAL

An STM32L476 application raises SYSCLK to 80 MHz from HSI16 and the main PLL, M 1, N 10, R 2,
in voltage range 1, the PWR_CR1 reset range, which `HAL_PWREx_ControlVoltageScaling` keeps.
`HAL_RCC_ClockConfig` sets the four wait states of RM0351 Table 11 first. Then it erases the last
2-Kbyte page of bank 2 with `HAL_FLASHEx_Erase`, PNB 63 with BKER, RM0351 Table 10's page 319 at
0x0803F800 on the 256-Kbyte part, which the firmware derives from the flash size. It programs
two double words into it with `HAL_FLASH_Program`, RM0351 3.3.6 and 3.3.7, and reads the option
bytes with `HAL_FLASHEx_OBGetConfig`, RDP level 0 at the production value.

The FLASH model holds BSY for the DS10198 Table 63 typical times, 22.02 ms per page erase and
81.69 us per double word, and checks LATENCY against RM0351 Table 11.
`MachineDescription(strict=True)` turns a LATENCY below the table into a `ModelError`.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/pwr_flash
```

## Read first

- `test_pwr_flash_l476.py`: `SystemCoreClock`, PWR_CR1 and FLASH_ACR at `clock_configured`, the
  page read through `machine.mem` before and after, the BSY times between marker calls, the
  option bytes through `machine.var` and the FLASH view, and a strict run failing at 0 wait
  states.
- `firmware/main.c`: the clock setup, the erase, the program and the option byte read.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/pwr_flash/pwr_flash.elf examples/stm32l476/pwr_flash/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.

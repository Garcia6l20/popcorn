# SPI NOR flash from FreeRTOS, behind a bootloader offset

An STM32U575 application reads the JEDEC ID of an AT25SF641 NOR flash, programs a 16-byte page
at 0x1000 and reads it back. The flash sits on SPI1 with the hardware SS output, PA4 to PA7. HAL
sets up the clock tree, the pins and SPI1. A FreeRTOS task then runs every instruction through
`spi_exchange`, an interrupt-driven transfer written on LL, and sleeps on a task notification
until the end of transfer. A second task counts scheduler ticks.

`spi_exchange` programs TSIZE and enables the TXP, RXP and EOT interrupts in a critical section,
before it sets SPE and starts the transfer. Hand-written drivers often use that order, and
RM0456 allows it: while SPE is clear, no SPI interrupt other than MODF reaches the NVIC.

The application is linked 0xC000 into flash, as if a bootloader started it. An 8-byte header
comes first and the vector table follows at 0x0800C008. VTOR cannot hold that address because
it is not 128-byte aligned. So `main` copies the table to an aligned SRAM array and points VTOR
there. FreeRTOS later reads its SVC and PendSV handlers from that table.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/spi
```

## Read first

- `popcorn.toml`: the chip, the prebuilt image, `[boot] vtor` entering the application's own
  vector table, and the flash on SPI1 with its page program time set to 800 us.
- `test_spi.py`: the `machine` fixture, the `popcorn` marker's instruction budget, runs to a
  symbol, intercepts that time two firmware hooks in virtual time, the DWARF view of
  `flash_result`, the flash contents, and a snapshot with its restore.
- `firmware/spi_exchange.c`: the transfer and its interrupt handler.
- `firmware/main.c`: the clock tree, SPI1 init, the flash task and the vector table relocation.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/spi/spi_flash.elf examples/stm32u575/spi/prebuilt/
```

Without the toolchain, the build uses the committed `prebuilt/spi_flash.elf`.

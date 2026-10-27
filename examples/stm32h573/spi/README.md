# SPI NOR flash on SPI4 from FreeRTOS, behind a bootloader offset

An STM32H573 application reads the JEDEC ID of an AT25SF321 NOR flash, programs a 16-byte page
at 0x1000 and reads it back. The flash sits on SPI4 with the hardware SS output, PE2 SCK, PE4
NSS, PE5 MISO and PE6 MOSI on AF5. HAL sets up the clock tree, the pins and SPI4. A FreeRTOS
task then runs every instruction through `spi_exchange`, written on LL: GPDMA1 channel 0 feeds
TXDR and channel 1 drains RXDR, and the task sleeps on a notification from the receive
channel's transfer-complete interrupt. A second task counts scheduler ticks.

It ports `stm32u575/spi`. The H5 moves the flash to SPI4 and the transfer from interrupt-driven
FIFO accesses to GPDMA requests 48 and 47, and the part is a 4 MiB AT25SF321 with JEDEC ID
1F 87 01.

`spi_exchange` programs TSIZE and the DMA requests, then sets SPE and starts the transfer. The
transmit channel raises no interrupt, so its transfer-complete flag stays set until the next
exchange arms the channel again.

The application is linked 0xC000 into flash, as if a bootloader started it. An 8-byte header
comes first and the vector table follows at 0x0800C008. VTOR cannot hold that address because
it is not aligned to the table size. So `main` copies the table to an aligned SRAM array and
points VTOR there. FreeRTOS later reads its SVC and PendSV handlers from that table.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/spi
```

## Read first

- `conftest.py`: the chip, the prebuilt image, `Boot(vtor=0x800C008)` entering the application's
  own vector table, and the 4 MiB flash on SPI4 with its page program time set to 800 us.
- `test_spi.py`: the `spi` fixture, the `popcorn` marker's instruction budget, runs to a
  symbol, intercepts that time two firmware hooks in virtual time, the DWARF view of
  `flash_result`, the flash contents, the pins and the GPDMA request selectors, and a snapshot
  with its restore.
- `firmware/spi_exchange.c`: the transfer and its interrupt handler.
- `firmware/main.c`: the clock tree, SPI4 init, the flash task and the vector table relocation.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/spi/spi_flash.elf examples/stm32h573/spi/prebuilt/
```

Without the toolchain, the build uses the committed `prebuilt/spi_flash.elf`.

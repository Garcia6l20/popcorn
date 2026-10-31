# printf banner and line echo on USART2, HAL

An STM32L152 application prints a banner with `printf` on USART2, 9600 8N1, PA2 TX and PA3 RX
on AF7, the ST-LINK virtual COM port of a Nucleo-L152RE. `_write` sends through
`HAL_UART_Transmit`, and stdout is line buffered, so each line leaves as back-to-back frames. It
then receives lines with `HAL_UARTEx_ReceiveToIdle_IT`. Each line, stripped of CR and LF, comes
back as `echo: <line>`. The main loop sleeps in WFI. Each test runs twice: on the xe build on an
STM32L152RETx and on the xb build on the Cat.1 STM32L152RBTx.

It ports `stm32u575/usart`. USART2 runs from PCLK1 at the 2.097 MHz MSI reset clock, so the
classic USART BRR, mantissa and fraction, is 218 and the rate 9619 baud.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/usart
```

## Read first

- `test_usart_l152.py`: `machine.periph.USART2.port` is the far end of the UART. `read_frames`
  returns each frame with the virtual time it ended, and the banner test checks the frame
  spacing against the rate BRR gives. The pin test reads GPIOA MODER and AFRL. `write` sends the
  host's line, and `read` collects the echo.
- `firmware/main.c`: the `_write` retarget, the receive-to-idle loop and the echo.
- `firmware/stm32l1xx_hal_msp.c`: the USART2 clock and the PA2 and PA3 pins.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/usart/usart_echo*.elf examples/stm32l152/usart/prebuilt/
```

The xe build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, the xb build
`../STM32L152XB_FLASH.ld`, 128 KB and 16 KB, so each stack lies inside the SRAM of every part it
runs on.

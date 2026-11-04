# printf banner and line echo on USART2, HAL

An STM32L476 application prints a banner with `printf` on USART2, 9600 8N1, PA2 TX and PA3 RX
on AF7, the ST-LINK virtual COM port of a Nucleo-L476RG. `_write` sends through
`HAL_UART_Transmit`, and stdout is line buffered, so each line leaves as back-to-back frames. It
then receives lines with `HAL_UARTEx_ReceiveToIdle_IT`. Each line, stripped of CR and LF, comes
back as `echo: <line>`. The main loop sleeps in WFI. The machine runs with `strict=True`.

It ports `stm32l152/usart`. USART2 runs from PCLK1 at the 4 MHz MSI reset clock, USART2SEL 00,
so BRR is 417 and the rate 9592 baud, RM0351 40.5.4.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/usart
```

## Read first

- `test_usart_l476.py`: `machine.periph.USART2.port` is the far end of the UART. `read_frames`
  returns each frame with the virtual time it ended, and the banner test checks the frame
  spacing against the rate BRR gives. The pin test reads GPIOA MODER and AFRL. `write` sends the
  host's line, and `read` collects the echo.
- `firmware/main.c`: the `_write` retarget, the receive-to-idle loop and the echo.
- `firmware/stm32l4xx_hal_msp.c`: the USART2 clock and the PA2 and PA3 pins.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/usart/usart_echo.elf examples/stm32l476/usart/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.

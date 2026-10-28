# printf banner and line echo on UART9, HAL

An STM32H573 application prints a banner with `printf` on UART9, 921600 8N1, PG1 TX and PG0 RX.
`_write` sends through `HAL_UART_Transmit`, and stdout is line buffered, so each line leaves as
back-to-back frames. It then receives lines with `HAL_UARTEx_ReceiveToIdle_IT`. Each line,
stripped of CR and LF, comes back as `echo: <line>`. The main loop sleeps in WFI.

It ports `stm32u575/usart`. UART9 sits on APB1 with its pins on AF11. At the 32 MHz reset clock
BRR is 35 and the real rate is 914286 baud, 0.8 % below 921600, well inside a UART receiver's
tolerance.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/usart
```

## Read first

- `test_usart_h573.py`: `machine.periph.UART9.port` is the far end of the UART. `read_frames`
  returns each frame with the virtual time it ended, and the banner test checks the frame
  spacing against the rate BRR gives, read back through `machine.periph.UART9.BRR`. The pin test
  reads GPIOG MODER and AFRL, and IDR following a driven PG0. `write` sends the host's line,
  and `read` collects the echo.
- `firmware/main.c`: the `_write` retarget, the receive-to-idle loop and the echo.
- `firmware/stm32h5xx_hal_msp.c`: the UART9 clock and the PG0 and PG1 pins.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/usart/usart_echo.elf examples/stm32h573/usart/prebuilt/
```

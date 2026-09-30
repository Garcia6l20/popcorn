# printf banner and line echo on USART1, HAL

An STM32U575 application prints a banner with `printf` on USART1, 115200 8N1, PA9 TX and PA10
RX. `_write` sends through `HAL_UART_Transmit`, and stdout is line buffered, so each line leaves
as back-to-back frames. It then receives lines with `HAL_UARTEx_ReceiveToIdle_IT`. Each line,
stripped of CR and LF, comes back as `echo: <line>`. The main loop sleeps in WFI.

The USART runs from PCLK2 at the 4 MHz reset clock, so BRR is 35 and the real rate is 114286
baud, 0.8 % below 115200, well inside a UART receiver's tolerance.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/usart
```

## Read first

- `test_usart.py`: `machine.periph.USART1.port` is the far end of the USART. `read_frames`
  returns each frame with the virtual time it ended, and the banner test checks the frame
  spacing against the rate BRR gives, read back through `machine.periph.USART1.BRR`. `write` sends the host's line, and `read` collects the echo.
- `firmware/main.c`: the `_write` retarget, the receive-to-idle loop and the echo.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/usart/usart_echo.elf examples/stm32u575/usart/prebuilt/
```

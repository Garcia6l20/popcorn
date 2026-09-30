# An AT modem mocked in Python on the host end of USART2, HAL IT

An STM32U575 application runs a small AT client on USART2, 115200 8N1, PA2 TX and PA3 RX. It
sends `AT`, `AT+CSQ` and `AT+SEND=hello`. Before each command it starts
`HAL_UARTEx_ReceiveToIdle_IT` for the reply, then waits in WFI for the idle event or a 100 ms
timeout of HAL ticks. `modem_result` records how each command was answered, the signal
quality `AT+CSQ` returned and the latency of the `AT` reply.

The modem is not a popcorn device. UARTs have no device class: the far end of a USART is its
port, `machine.periph.USART2.port`, a byte stream in virtual time. `atmodem.AtModem` is a
plain Python class written next to the test that reads command lines from the port, waits a
reply delay by running the machine, and writes the scripted reply. It all runs in the test's
coroutine, without a host thread.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/uart_mock
```

## Read first

- `atmodem.py`: the whole mock. `read_command` collects a line with `port.read_frames`,
  `serve` answers each command after `await machine.run(for_=delay)` with
  `await port.write(reply)`. A command mapped to None gets no answer, as a hung modem.
  `commands` and `answered` record the virtual times.
- `test_uart_mock.py`:
  - every command answered: the three results OK, the signal quality 17, the `AT` latency
    5 or 6 ms, each reply 5 ms after its command's last byte, within 10 us;
  - an `ERROR` reply to `AT+SEND`;
  - no reply to `AT+CSQ`: the firmware times out after 100 ms and the session goes on.
- `firmware/main.c`: the receive-to-idle wait with its timeout, the reply parsing.

The port records from machine build, so the modem does not need to take it before the
firmware runs.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/uart_mock/at_client.elf examples/stm32u575/uart_mock/prebuilt/
```

"""An AT modem mocked in Python on the host end of USART2, 115200 8N1, HAL interrupt receive.

The firmware sends `AT`, `AT+CSQ` and `AT+SEND=hello`, each followed by a receive-to-idle of
the reply with a 100 ms timeout, and records how each was answered in `modem_result`.
`atmodem.AtModem` reads the commands from `machine.periph.USART2.port` and answers from a
script 5 ms of virtual time after each command's last byte, in the test's own coroutine. The
firmware sleeps in WFI while it waits for a reply, and a time stop from an idle core lands less
than one instruction after its deadline (`Machine.run`). At 4 MHz that is under 250 ns, inside
the 10 us the test allows.

USART2 runs from PCLK1 at the 4 MHz reset clock, so BRR is 35 and a byte takes 87.5 us.
`at_latency_ms` is the HAL tick count from the end of `AT` to the end of its reply: the 5 ms
delay, 4 bytes of `OK\r\n`, the idle line and the tick rounding make it 5 or 6.
"""

from collections.abc import Mapping

import pytest
from atmodem import AtModem
from popcorn import Machine, StopReason
from popcorn.units import ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

OK = 1
ERROR = 2
TIMEOUT = 3

HAPPY = {
    b"AT": b"OK\r\n",
    b"AT+CSQ": b"+CSQ: 17,99\r\n\r\nOK\r\n",
    b"AT+SEND=hello": b"OK\r\n",
}


def modem(machine: Machine, replies: Mapping[bytes, bytes | None]) -> AtModem:
    return AtModem(machine, machine.periph.USART2.port, replies, delay=ms(5))


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="modem_done", for_=ms(500))
    assert stop.reason is StopReason.ADDRESS, stop


async def test_every_command_is_answered(uart_mock: Machine) -> None:
    at = modem(uart_mock, HAPPY)
    await at.serve(3)
    await run_to_done(uart_mock)

    result = uart_mock.var("modem_result")
    assert [command for command, _ in at.commands] == list(HAPPY)
    assert (result.at.value, result.csq.value, result.send.value) == (OK, OK, OK)
    assert result.rssi.value == 17
    assert 5 <= result.at_latency_ms.value <= 6
    for (_, asked), (_, answered) in zip(at.commands, at.answered, strict=True):
        assert ms(5) <= answered - asked < ms(5) + us(10)


async def test_an_error_reply_is_reported(uart_mock: Machine) -> None:
    at = modem(uart_mock, {**HAPPY, b"AT+SEND=hello": b"ERROR\r\n"})
    await at.serve(3)
    await run_to_done(uart_mock)
    result = uart_mock.var("modem_result")
    assert result.send.value == ERROR
    assert result.at.value == OK


async def test_a_reply_that_never_comes_times_out_and_the_session_goes_on(
    uart_mock: Machine,
) -> None:
    at = modem(uart_mock, {**HAPPY, b"AT+CSQ": None})
    await at.serve(3)
    await run_to_done(uart_mock)

    result = uart_mock.var("modem_result")
    assert result.csq.value == TIMEOUT
    assert result.rssi.value == -1
    assert result.send.value == OK
    (_, csq_at), (_, send_at) = at.commands[1:]
    assert send_at - csq_at >= ms(100)

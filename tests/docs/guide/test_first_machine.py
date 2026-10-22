import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/usart/prebuilt/usart_echo.elf"

# --8<-- [start:describe]
ECHO = popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(ELF)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)
# --8<-- [end:describe]


async def test_a_machine_boots_echoes_and_shows_its_state() -> None:
    # --8<-- [start:boot]
    machine = ECHO.machine()
    stop = await ECHO.boot_machine(machine)
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert stop.location.symbol == "main"
    # --8<-- [end:boot]

    # --8<-- [start:uart]
    port = machine.periph.USART1.port
    banner = await port.read(36, for_=ms(10))
    assert banner == b"popcorn usart example, 115200 baud\r\n"

    await port.write(b"hi\r\n")
    assert await port.read(10, for_=ms(10)) == b"echo: hi\r\n"
    # --8<-- [end:uart]

    # --8<-- [start:state]
    line = machine.var("line")
    assert bytes(int(line[i].value) for i in range(2)) == b"hi"
    assert machine.var("received").value == 0
    assert machine.periph.USART1.BRR.value == 35
    assert machine.reg.sp != 0
    # --8<-- [end:state]


# --8<-- [start:script]
async def main() -> None:
    machine = ECHO.machine()
    await ECHO.boot_machine(machine)
    banner = await machine.periph.USART1.port.read(36, for_=ms(10))
    print(banner.decode(), end="")


if __name__ == "__main__":
    asyncio.run(main())
# --8<-- [end:script]


async def test_the_script_form_prints_the_banner(capsys: pytest.CaptureFixture[str]) -> None:
    await main()
    assert capsys.readouterr().out == "popcorn usart example, 115200 baud\r\n"

# --8<-- [start:describe]
from pathlib import Path

import popcorn
from popcorn.units import ms

ELF = Path(__file__).parents[3] / "examples/stm32u575/usart/prebuilt/usart_echo.elf"

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

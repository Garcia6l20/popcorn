"""MachineDescription of the spi example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.devices import At25sf
from popcorn.units import us

HERE = Path(__file__).parent

SPI = popcorn.MachineDescription(
    name="spi-flash",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/spi_flash.elf")],
    boot=popcorn.Boot(vtor=0x800C008, until="main", budget=1_000_000),
    board={
        "SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, page_program=us(800))]),
    },
)


@popcorn.pytest.machine
def spi() -> popcorn.MachineDescription:
    return SPI

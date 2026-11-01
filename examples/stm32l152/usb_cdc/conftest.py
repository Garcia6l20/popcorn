"""MachineDescription of the usb_cdc example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import RccOptions, UsbHostOptions

HERE = Path(__file__).parent

USB_CDC = popcorn.MachineDescription(
    name="usb-cdc",
    part="STM32L152RETx",
    images=[popcorn.Elf(HERE / "prebuilt/usb_cdc.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    rcc=RccOptions(hse_hz=8_000_000),
    board={
        "USB": popcorn.Periph(host=UsbHostOptions()),
    },
)


@popcorn.pytest.machine
def usb_cdc() -> popcorn.MachineDescription:
    return USB_CDC

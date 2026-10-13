"""MachineDescription of the usb_audio example."""

from pathlib import Path

import popcorn
import pytest
from popcorn.board import UsbHostOptions

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="usb-audio",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/usb_audio.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "OTG_FS": popcorn.Periph(host=UsbHostOptions()),
    },
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.MachineDescription:
    return PROJECT

"""MachineDescription of the sai_codec example."""

from pathlib import Path

import popcorn
import popcorn.pytest
from popcorn.board import AudioOptions
from stereocodec import StereoCodec

HERE = Path(__file__).parent

PROJECT = popcorn.MachineDescription(
    name="sai-codec",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/sai_codec.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "I2C1": popcorn.Periph(devices=[popcorn.Device(StereoCodec, address=0x1A)]),
        "SAI1": popcorn.Periph(audio={"A": AudioOptions(clocked_by="I2C1:0x1A")}),
    },
)


@popcorn.pytest.machine
def sai_codec() -> popcorn.MachineDescription:
    return PROJECT

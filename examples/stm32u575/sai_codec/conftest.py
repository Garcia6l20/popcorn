"""Project of the sai_codec example."""

from pathlib import Path

import popcorn
import pytest
from popcorn.board import AudioOptions
from stereocodec import StereoCodec

HERE = Path(__file__).parent

PROJECT = popcorn.Project(
    name="sai-codec",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/sai_codec.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    board={
        "I2C1": popcorn.Periph(devices=[popcorn.Device(StereoCodec, address=0x1A)]),
        "SAI1": popcorn.Periph(audio={"A": AudioOptions(clocked_by="I2C1:0x1A")}),
    },
)


@pytest.fixture(scope="session")
def popcorn_project() -> popcorn.Project:
    return PROJECT

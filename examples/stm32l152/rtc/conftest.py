"""MachineDescription of the rtc example."""

from pathlib import Path

import popcorn
import popcorn.pytest
import pytest

HERE = Path(__file__).parent

RTC = popcorn.MachineDescription(
    name="rtc",
    part="STM32L152RETx",
    images=[popcorn.Elf(HERE / "prebuilt/rtc_calendar.elf")],
    boot=popcorn.Boot(until="rtc_ready", budget=2_000_000),
    strict=True,
)


@popcorn.pytest.machine
def rtc() -> popcorn.MachineDescription:
    return RTC


@pytest.fixture(scope="session")
def rtc_description() -> popcorn.MachineDescription:
    return RTC

"""MachineDescription of the native_plugin example: a plugin model on RNG, a plugin device on
SPI1."""

from pathlib import Path
from typing import TYPE_CHECKING

import popcorn
import popcorn.pytest
import pytest
from popcorn.devices import chip_select
from popcorn.plugin import Plugin

HERE = Path(__file__).parent

PLUGIN = Plugin.dev(HERE / "plugin")

if TYPE_CHECKING:
    from popcorn.devices import SpiTargetBase

    class AdcBase(SpiTargetBase):
        """What `plugin/bind.cpp` binds, declared for pyright: a plugin ships no stub."""

        def __init__(self, counts_per_ms: int) -> None: ...
        @property
        def counts_per_ms(self) -> int: ...
        @property
        def selects(self) -> int: ...
        @property
        def deselects(self) -> int: ...
        @property
        def bad_frames(self) -> int: ...

else:
    AdcBase = PLUGIN.module().AdcBase


class Adc(AdcBase):
    """The C++ ADC of the plugin on chip select `cs`, its ramp rising `counts_per_ms` counts per
    millisecond. Read its counters while the machine is parked."""

    def __init__(self, cs: str | None = None, counts_per_ms: int = 10) -> None:
        super().__init__(counts_per_ms)
        self._cs_port, self._cs_pin = chip_select(cs)


NATIVE_PLUGIN = popcorn.MachineDescription(
    name="native-plugin",
    part="STM32U575QGIx",
    images=[popcorn.Elf(HERE / "prebuilt/native_plugin.elf")],
    boot=popcorn.Boot(until="main", budget=1_000_000),
    plugins=[PLUGIN],
    board={
        "RNG": popcorn.Periph(model=PLUGIN.model("rng")),
        "SPI1": popcorn.Periph(devices=[popcorn.Device(Adc, cs="PB0", counts_per_ms=25)]),
    },
)


@popcorn.pytest.machine
def native_plugin() -> popcorn.MachineDescription:
    return NATIVE_PLUGIN


@pytest.fixture(scope="session")
def native_plugin_description() -> popcorn.MachineDescription:
    return NATIVE_PLUGIN

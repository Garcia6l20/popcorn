"""A device class of a dev plugin's bindings module, attached to a machine, run by firmware."""

from __future__ import annotations

import shutil
import types
from collections.abc import Iterator
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import _core
from popcorn._devices import chip_select
from popcorn.plugin import Plugin

from .test_devices import SPI_ELF, enter, finish

ECHO = Path(__file__).resolve().parents[1] / "native_plugin" / "fixtures" / "echo"


@pytest.fixture(scope="module")
def echo_module(tmp_path_factory: pytest.TempPathFactory) -> Iterator[types.ModuleType]:
    root = tmp_path_factory.mktemp("plugin-device")
    source = root / "echo"
    shutil.copytree(ECHO, source)
    with pytest.MonkeyPatch.context() as patch:
        patch.setenv("POPCORN_PLUGIN_CACHE", str(root / "cache"))
        yield Plugin.dev(source).module()


def test_a_plugin_device_exchanges_frames_with_the_firmware(
    u575: popcorn.PartDescription, echo_module: types.ModuleType
) -> None:
    base = cast(type[_core.SpiTargetBase], echo_module.EchoBase)

    class Echo(base):
        def __init__(self) -> None:
            super().__init__()
            self._cs_port, self._cs_pin = chip_select("PA4")

    device = Echo()
    devices = cast(Any, {"SPI1": [device]})
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices=devices)
    enter(m, "poll")
    assert finish(m) == [0xA0, 0x01, 0x01, 0x01]
    assert m.board.devices("SPI1") == (device,)

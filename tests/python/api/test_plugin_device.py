"""A device class of a dev plugin's bindings module, attached to a machine, run by firmware."""

from __future__ import annotations

import shutil
import types
from collections.abc import Iterator
from pathlib import Path
from typing import Any, cast

import popcorn
import popcorn.devices
import pytest
from popcorn import ConfigError, _core
from popcorn.board import AudioOptions, SpiTarget
from popcorn.devices import chip_select
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


def echo_on(echo_module: types.ModuleType, cs: str | None) -> _core.SpiTargetBase:
    base = cast(type[_core.SpiTargetBase], echo_module.EchoBase)

    class Echo(base):
        def __init__(self) -> None:
            super().__init__()
            self._cs_port, self._cs_pin = chip_select(cs)

    return Echo()


def test_a_plugin_device_exchanges_frames_with_the_firmware(
    u575: popcorn.PartDescription, echo_module: types.ModuleType
) -> None:
    device = echo_on(echo_module, "PA4")
    devices = cast(Any, {"SPI1": [device]})
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices=devices)
    enter(m, "poll")
    assert finish(m) == [0xA0, 0x01, 0x01, 0x01]
    assert m.board.devices("SPI1") == (device,)


@pytest.mark.parametrize(
    ("cs", "expected"),
    [("PA4", ("GPIOA", 4)), ("PB0", ("GPIOB", 0)), ("PH15", ("GPIOH", 15)), (None, ("", None))],
)
def test_chip_select_names_the_port_and_pin_the_machine_reads(
    cs: str | None, expected: tuple[str, int | None]
) -> None:
    assert chip_select(cs) == expected


@pytest.mark.parametrize("cs", ["PA16", "A4", "pa4", "PA", ""])
def test_chip_select_refuses_what_is_not_a_pin_name(cs: str) -> None:
    with pytest.raises(ValueError, match="a chip select is a pin name"):
        chip_select(cs)


def test_chip_select_is_public() -> None:
    assert "chip_select" in popcorn.devices.__all__


def test_clocked_by_finds_a_plugin_device_by_its_chip_select(
    u575: popcorn.PartDescription, echo_module: types.ModuleType
) -> None:
    on_pin, on_ss = echo_on(echo_module, "PB0"), SpiTarget()
    devices = {"SPI1": [on_pin, on_ss]}
    m = popcorn.Machine(
        u575,
        devices=devices,
        audio={
            "SAI1": {
                "A": AudioOptions(clocked_by="SPI1:PB0"),
                "B": AudioOptions(clocked_by="SPI1:SS"),
            }
        },
    )
    on_ss.board.set_audio_clock("SAI1", "B", 44_100)
    assert m.periph.SAI1.audio("B").external_frame_hz == 44_100
    with pytest.raises(ConfigError):
        on_ss.board.set_audio_clock("SAI1", "A", 48_000)


def test_clocked_by_ss_does_not_name_a_plugin_device_on_a_pin(
    u575: popcorn.PartDescription, echo_module: types.ModuleType
) -> None:
    devices = {"SPI1": [echo_on(echo_module, "PB0")]}
    with pytest.raises(ConfigError) as refused:
        popcorn.Machine(
            u575, devices=devices, audio={"SAI1": {"A": AudioOptions(clocked_by="SPI1:SS")}}
        )
    assert refused.value.key == "audio.SAI1.A.clocked_by"


def test_a_plugin_device_on_a_missing_gpio_port_is_refused(
    u575: popcorn.PartDescription, echo_module: types.ModuleType
) -> None:
    devices = {"SPI1": [echo_on(echo_module, "PZ4")]}
    with pytest.raises(ConfigError) as refused:
        popcorn.Machine(u575, devices=devices)
    assert refused.value.key == "devices.SPI1"
    assert refused.value.expected == "a chip select on a GPIO port of STM32U575QGIx"


@pytest.mark.parametrize("name", ["SpiTargetBase", "I2CTargetBase", "SpiControllerBase"])
def test_the_native_device_bases_are_public(name: str) -> None:
    assert name in popcorn.devices.__all__
    assert getattr(popcorn.devices, name) is getattr(_core, name)


def test_a_plugin_device_is_an_instance_of_the_public_base(
    echo_module: types.ModuleType,
) -> None:
    assert issubclass(echo_module.EchoBase, popcorn.devices.SpiTargetBase)
    assert isinstance(echo_on(echo_module, "PA4"), popcorn.devices.SpiTargetBase)

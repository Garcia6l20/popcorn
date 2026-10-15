"""A device type bound in another nanobind module than `_core`, derived from `_core`'s base.

Each test imports `popcorn_testing._xmod` itself, so a module that cannot find its base fails
the tests that import it rather than the collection. `plugin::require_core` turns a module
linking its own core or built in another nanobind domain into an ImportError, the variants under
`popcorn_testing/xmod_static/` and `popcorn_testing/xmod_domain/` prove both.
"""

from __future__ import annotations

import gc
import subprocess
import sys
import sysconfig
import weakref
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

import popcorn
import pytest
from popcorn import ConfigError, _core
from popcorn._devices import chip_select

from .test_devices import SPI_ELF, enter, finish

if TYPE_CHECKING:
    from popcorn_testing._xmod import EchoTarget


def echo(cs: str | None = "PA4") -> EchoTarget:
    """A Python subclass of `EchoTarget`, carrying its chip select and an extra attribute."""
    from popcorn_testing._xmod import EchoTarget

    class Echo(EchoTarget):
        def __init__(self) -> None:
            super().__init__()
            self._cs_port, self._cs_pin = chip_select(cs)
            self.label = "echo"

    return Echo()


def echo_machine(u575: popcorn.PartDescription, device: EchoTarget) -> popcorn.Machine:
    devices = cast(Any, {"SPI1": [device]})
    return popcorn.Machine(u575, image=popcorn.Image.elf(SPI_ELF), devices=devices)


def test_the_type_derives_from_the_core_base() -> None:
    from popcorn_testing._xmod import EchoTarget

    assert isinstance(EchoTarget(), _core.SpiTargetBase)
    assert issubclass(EchoTarget, _core.SpiTargetBase)


def test_a_python_subclass_keeps_its_attributes() -> None:
    from popcorn_testing._xmod import EchoTarget

    device = echo()
    assert isinstance(device, EchoTarget)
    assert isinstance(device, _core.SpiTargetBase)
    assert vars(device)["label"] == "echo"
    assert device.frames == 0


def test_the_firmware_exchanges_frames_with_the_device(u575: popcorn.PartDescription) -> None:
    device = echo()
    m = echo_machine(u575, device)
    enter(m, "poll")
    assert finish(m) == [0xA0, 0x01, 0x01, 0x01]
    assert device.frames == 4
    assert device.selects == 1
    assert m.board.devices("SPI1") == (device,)


def test_a_snapshot_restores_the_device_count(u575: popcorn.PartDescription) -> None:
    device = echo()
    m = echo_machine(u575, device)
    enter(m, "poll")
    finish(m)
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    copy = echo()
    other = echo_machine(u575, copy)
    assert copy.frames == 0
    other.restore(saved)
    assert copy.frames == 4


def test_a_config_error_thrown_in_the_module_is_a_config_error() -> None:
    from popcorn_testing._xmod import throw_config

    with pytest.raises(ConfigError) as raised:
        throw_config()
    assert raised.value.key == "field"
    assert raised.value.expected == "expected"


def test_the_machine_and_the_device_are_collected(u575: popcorn.PartDescription) -> None:
    device = echo()
    m = echo_machine(u575, device)
    enter(m, "poll")
    finish(m)
    machine_ref = weakref.ref(m)
    device_ref = weakref.ref(device)
    del m, device
    gc.collect()
    assert machine_ref() is None
    assert device_ref() is None


def test_the_stub_names_the_core_base() -> None:
    import popcorn_testing._xmod as xmod

    assert xmod.__file__ is not None
    stub = Path(xmod.__file__).with_name("_xmod.pyi").read_text()
    assert "class EchoTarget(popcorn._core.SpiTargetBase):" in stub


def test_the_module_sees_the_registry_of_core() -> None:
    from popcorn_testing._xmod import registered_glob

    assert registered_glob("RCC", "STM32U5_rcc_x") == "STM32U5_rcc_*"


def variant(package: str) -> subprocess.CompletedProcess[str]:
    """Import the `_xmod` built as build/popcorn_testing/@p package, exit 3 with an ImportError."""
    import popcorn_testing._xmod

    directory = Path(popcorn_testing._xmod.__file__).resolve().parent / package
    path = directory / f"_xmod{sysconfig.get_config_var('EXT_SUFFIX')}"
    assert path.is_file(), path
    name = f"popcorn_testing.{package}._xmod"
    probe = (
        "import importlib.util, sys\n"
        "import popcorn._core\n"
        f"spec = importlib.util.spec_from_file_location({name!r}, {str(path)!r})\n"
        "try:\n"
        "    module = importlib.util.module_from_spec(spec)\n"
        "    spec.loader.exec_module(module)\n"
        "except ImportError as error:\n"
        "    print(error)\n"
        "    raise SystemExit(3)\n"
    )
    return subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True)


def test_a_module_linking_its_own_core_fails_its_import() -> None:
    ran = variant("xmod_static")
    assert ran.returncode == 3, ran.stderr
    assert "links its own copy of popcorn core" in ran.stdout
    assert "libpopcorn_core.so" in ran.stdout


def test_a_module_in_another_nanobind_domain_fails_its_import() -> None:
    ran = variant("xmod_domain")
    assert ran.returncode == 3, ran.stderr
    assert "does not share popcorn._core's nanobind types" in ran.stdout
    assert "Critical nanobind error" not in ran.stderr

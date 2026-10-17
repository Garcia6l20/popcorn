"""Plugins on a machine description: `plugins=`, `Periph(model=PLUGIN.model(name))`, the device
rule, the digest and the boot cache.

The `kit` fixture plugin registers `rng`, a counter on DR, and `uart`, a UART implementing only
`periph::uart_port` on TDR and RDR, and binds the SPI target `KitTargetBase`. A loaded plugin is
never unloaded, so the in-process cases share one build of the fixture directory per module,
and the cases that must start without it run in their own interpreter.
"""

from __future__ import annotations

import asyncio
import gc
import os
import shutil
import subprocess
import sys
import textwrap
import weakref
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import assert_type, cast

import popcorn
import pytest
from popcorn import ConfigError, ModelError, _core
from popcorn import _machine_description as machine_description
from popcorn.board import AudioOptions, RccOptions, SpiTarget, UsbHostOptions
from popcorn.devices import chip_select
from popcorn.model import Model, on_read
from popcorn.plugin import Plugin, PluginModel, _records
from popcorn.pytest._cache import cache_key
from popcorn.units import us

from .support import FIXTURES, copied, run_python

ROOT = Path(__file__).resolve().parents[3]
BOARD_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "board.elf"
PART = "STM32U575QGIx"
RNG_BASE = 0xC0DE_0000
RDR_EMPTY = 0xDEAD

BEFORE_PLUGINS_DIGEST = "7f7722f5eb5fb97ebe085f6810a7310c132a087943a3bc68c34e8e36e537ed3c"


@pytest.fixture(scope="module", autouse=True)
def u575() -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        mcu(PART)
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


@pytest.fixture(scope="module")
def kit(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Plugin]:
    """The `kit` fixture plugin, built into a cache of this module."""
    with pytest.MonkeyPatch.context() as patch:
        patch.setenv("POPCORN_PLUGIN_CACHE", str(tmp_path_factory.mktemp("kit-cache")))
        yield Plugin.dev(FIXTURES / "kit")


class Fixed(Model):
    """A Python model answering 0x1234 on DR."""

    @on_read("DR")
    def _dr(self) -> int:
        return 0x1234


class Plain(Model):
    """A Python model claiming no register."""


def described(
    kit: Plugin | None = None, images: bool = False, **board: popcorn.Periph
) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        part=PART,
        images=[popcorn.Elf(BOARD_ELF)] if images else [],
        plugins=[] if kit is None else [kit],
        board=board,
    )


def kit_device(kit: Plugin) -> type[_core.SpiTargetBase]:
    """A Python subclass of the plugin's `KitTargetBase` taking a chip select."""
    return taking_cs(cast(type[_core.SpiTargetBase], kit.module().KitTargetBase))


def taking_cs(base: type[_core.SpiTargetBase]) -> type[_core.SpiTargetBase]:
    """A Python subclass of @p base taking a chip select."""

    class KitDevice(base):
        def __init__(self, cs: str | None = None) -> None:
            super().__init__()
            if cs is not None:
                self._cs_port, self._cs_pin = chip_select(cs)

    return KitDevice


def kit_instance(device: type[_core.SpiTargetBase], cs: str) -> _core.SpiTargetBase:
    return cast(Callable[..., _core.SpiTargetBase], device)(cs=cs)


def refused(make: Callable[[], object]) -> ConfigError:
    with pytest.raises(ConfigError) as caught:
        make()
    return caught.value


def no_machine(*_: object, **__: object) -> popcorn.Machine:
    raise AssertionError("a Machine was built")


def no_build(self: Plugin) -> Path:
    raise AssertionError(f"plugin {self.name} was built")


def rng_reads(m: popcorn.Machine, count: int) -> list[int]:
    return [m.periph.RNG.DR.read() for _ in range(count)]


def test_a_model_name_is_checked_at_the_call(kit: Plugin) -> None:
    assert kit.model("rng") == PluginModel(kit, "rng")
    assert refused(lambda: kit.model("")).key == "model"
    assert refused(lambda: kit.model(cast(str, 3))).key == "model"
    error = refused(lambda: PluginModel(cast(Plugin, "kit"), "rng"))
    assert (error.key, error.expected) == ("plugin", "a popcorn.plugin.Plugin")
    assert refused(lambda: PluginModel(cast(Plugin, "kit"), "")).key == "plugin"


def test_a_plugin_model_names_a_plugin_in_plugins(kit: Plugin) -> None:
    error = refused(lambda: described(RNG=popcorn.Periph(model=kit.model("rng"))))
    assert error.key == "board.RNG.model"
    assert error.expected == "a model of a plugin in plugins, not of plugin kit"


def test_two_plugins_of_one_name_are_refused(kit: Plugin) -> None:
    other = Plugin.dev(FIXTURES / "echo", name="kit")
    error = refused(lambda: popcorn.MachineDescription(part=PART, images=[], plugins=[kit, other]))
    assert error.key == "plugins[1]"


def test_a_plugin_model_serves_only_the_peripheral_the_board_names(kit: Plugin) -> None:
    named = described(kit, RNG=popcorn.Periph(model=kit.model("rng"))).machine()
    declared = described(kit).machine()
    plain = described().machine()
    assert rng_reads(named, 3) == [RNG_BASE, RNG_BASE + 1, RNG_BASE + 2]
    for m in (declared, plain):
        with pytest.raises(popcorn.ModelError):
            rng_reads(m, 1)
        assert m.periph.RNG.DR.value == 0
    assert rng_reads(named, 1) == [RNG_BASE + 3]


def test_a_plugin_model_on_one_instance_leaves_the_others_built_in(kit: Plugin) -> None:
    m = described(kit, USART2=popcorn.Periph(model=kit.model("uart"))).machine()
    assert m.periph.USART2.RDR.read() == RDR_EMPTY
    assert m.periph.USART1.RDR.read() == 0
    assert m.periph.USART3.RDR.read() == 0


def test_the_plugin_model_wins_over_a_registered_class(kit: Plugin, tmp_path: Path) -> None:
    library = kit.built()
    ran = run_python(
        f"""
        import popcorn
        from popcorn.model import Model, on_read, register
        from popcorn.plugin import Plugin

        @register("RNG")
        class Registered(Model):
            @on_read("DR")
            def _dr(self) -> int:
                return 0x1234

        kit = Plugin("kit", library=__import__("pathlib").Path({str(library)!r}))
        def machine(**board):
            return popcorn.MachineDescription(
                part="{PART}", images=[], plugins=[kit], board=board
            ).machine()
        print(hex(machine(RNG=popcorn.Periph(model=kit.model("rng"))).periph.RNG.DR.read()))
        print(hex(machine().periph.RNG.DR.read()))
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.split() == [hex(RNG_BASE), "0x1234"]


def test_an_unknown_model_name_is_refused_before_the_machine(
    kit: Plugin, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(machine_description, "Machine", no_machine)
    description = described(kit, RNG=popcorn.Periph(model=kit.model("nope")))
    error = refused(description.machine)
    assert error.key == "board.RNG.model"
    assert error.expected == "a model plugin kit registers, one of rng, uart"


def test_a_peripheral_the_part_lacks_is_refused_before_any_build(
    kit: Plugin, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(Plugin, "built", no_build)
    description = described(kit, NOPE=popcorn.Periph(model=kit.model("rng")))
    error = refused(description.machine)
    assert error.key == "board.NOPE"
    assert error.expected == f"a peripheral of {PART}"


def test_a_device_of_an_undeclared_plugin_is_refused_at_construction(kit: Plugin) -> None:
    base = cast(type, kit.module().KitTargetBase)
    error = refused(
        lambda: described(SPI1=popcorn.Periph(devices=[popcorn.Device(base, cs="PA4")]))
    )
    assert error.key == "board.SPI1.devices[0]"
    assert "of plugin kit" in error.expected


def test_a_python_subclass_of_a_plugin_base_needs_its_plugin(kit: Plugin) -> None:
    device = kit_device(kit)
    board = popcorn.Periph(devices=[popcorn.Device(device, cs="PA4")])
    error = refused(lambda: described(SPI1=board))
    assert error.key == "board.SPI1.devices[0]"
    assert "derives from popcorn_plugins.kit._native:KitTargetBase of plugin kit" in error.expected
    m = described(kit, SPI1=board).machine()
    assert type(m.board.devices("SPI1")[0]) is device
    error = refused(lambda: described().machine(devices={"SPI1": [kit_instance(device, "PA4")]}))
    assert error.key == "devices.SPI1[0]"
    assert described(kit).machine(devices={"SPI1": [kit_instance(device, "PA4")]}) is not None


def test_a_python_subclass_copying_a_base_new_is_not_bound_natively() -> None:
    class FromObject(SpiTarget):
        __new__ = object.__new__

    class FromBase(SpiTarget):
        __new__ = _core.SpiTargetBase.__new__

    for cls in (FromObject, FromBase):
        board = popcorn.Periph(devices=[popcorn.Device(cls, cs="PA4")])
        assert described(SPI1=board).board["SPI1"].devices[0].cls is cls


def test_a_plugin_device_reads_back_typed_without_a_cast(kit: Plugin) -> None:
    device = kit_device(kit)
    m = described(kit, SPI1=popcorn.Periph(devices=[popcorn.Device(device, cs="PA4")])).machine()
    found = m.board.device("SPI1", device)
    assert_type(found, _core.SpiTargetBase)
    assert m.board.devices("SPI1") == (found,)
    assert type(found) is device


def test_a_device_bound_in_a_submodule_of_the_bindings_names_its_plugin(kit: Plugin) -> None:
    base = cast(type[_core.SpiTargetBase], kit.module().spi.KitSubBase)
    assert base.__module__ == "popcorn_plugins.kit._native.spi"
    device = taking_cs(base)
    board = popcorn.Periph(devices=[popcorn.Device(device, cs="PA4")])
    m = described(kit, SPI1=board).machine()
    assert type(m.board.devices("SPI1")[0]) is device
    error = refused(lambda: described(SPI1=board))
    assert error.key == "board.SPI1.devices[0]"
    assert error.expected.endswith("popcorn_plugins.kit._native.spi:KitSubBase of plugin kit")


def test_a_device_from_a_module_plugin_module_did_not_import_is_refused(
    kit: Plugin, tmp_path: Path
) -> None:
    source = copied("kit", tmp_path)
    ran = run_python(
        f"""
        import importlib.util
        import popcorn
        from popcorn import ConfigError
        from popcorn.plugin import Plugin, _build_cache
        built = _build_cache.build("kit", __import__("pathlib").Path({str(source)!r}))
        spec = importlib.util.spec_from_file_location("popcorn_plugins.x._native", built.bindings)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        try:
            popcorn.MachineDescription(
                part="{PART}",
                images=[],
                plugins=[Plugin.dev({str(source)!r})],
                board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(module.KitTargetBase)])}},
            )
        except ConfigError as error:
            print(error.key)
            print(error.expected)
            raise SystemExit(3)
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 3, ran.stderr
    key, expected = ran.stdout.splitlines()[:2]
    assert key == "board.SPI1.devices[0]"
    assert expected.endswith("from popcorn_plugins.x._native")


def test_a_library_naming_another_plugin_is_refused_by_machine(kit: Plugin) -> None:
    other = Plugin.dev(FIXTURES / "kit", name="other")
    error = refused(described(other).machine)
    assert error.key == "plugins[0]"
    assert "POPCORN_PLUGIN kit, not other" in str(error)


def test_a_plugin_factory_the_python_check_missed_is_refused_natively(kit: Plugin) -> None:
    loaded = _records.load(kit, kit.built())
    description = described().part_description()
    error = refused(lambda: popcorn.Machine(description, _plugin_models={"RNG": (loaded, "nope")}))
    assert error.key == "board.RNG.model"
    assert error.expected == "a model plugin kit registers, one of rng, uart"


def model_of(kind: str, kit: Plugin, cls: type[Model] = Plain) -> popcorn.Use | PluginModel:
    return popcorn.Use(cls) if kind == "Python" else kit.model("rng")


KINDS = ["Python", "plugin"]


@pytest.mark.parametrize("kind", KINDS)
def test_board_devices_beside_a_board_model_are_refused_at_construction(
    kit: Plugin, kind: str
) -> None:
    periph = popcorn.Periph(
        model=model_of(kind, kit), devices=[popcorn.Device(SpiTarget, cs="PA4")]
    )
    error = refused(lambda: described(kit, SPI1=periph))
    assert error.key == "board.SPI1.devices"
    assert error.expected == f"no devices on a peripheral that has a {kind} model"


@pytest.mark.parametrize("kind", KINDS)
def test_machine_devices_beside_a_board_model_are_refused_before_the_handle(
    kit: Plugin, kind: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    description = described(kit, SPI1=popcorn.Periph(model=model_of(kind, kit)))
    monkeypatch.setattr(_core, "MachineHandle", no_machine)
    error = refused(lambda: description.machine(devices={"SPI1": [SpiTarget(cs="PA4")]}))
    assert error.key == "devices.SPI1"
    assert error.expected == f"a peripheral without a {kind} model"


@pytest.mark.parametrize("kind", KINDS)
def test_rcc_options_beside_a_board_model_on_rcc_are_refused(kit: Plugin, kind: str) -> None:
    description = described(kit, RCC=popcorn.Periph(model=model_of(kind, kit))).replace(
        rcc=RccOptions()
    )
    error = refused(description.machine)
    assert error.key == "rcc"
    assert error.expected == f"an RCC without a {kind} model"


@pytest.mark.parametrize("kind", KINDS)
def test_a_usb_host_beside_a_board_model_is_refused_before_any_build(
    kit: Plugin, kind: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(machine_description, "Machine", no_machine)
    monkeypatch.setattr(Plugin, "built", no_build)
    periph = popcorn.Periph(model=model_of(kind, kit), host=UsbHostOptions())
    error = refused(described(kit, OTG_FS=periph).machine)
    assert error.key == "board.OTG_FS.host"
    assert error.expected == "a USB device controller with the built-in model"


@pytest.mark.parametrize("kind", KINDS)
def test_audio_beside_a_board_model_is_refused_before_any_build(
    kit: Plugin, kind: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(machine_description, "Machine", no_machine)
    monkeypatch.setattr(Plugin, "built", no_build)
    periph = popcorn.Periph(model=model_of(kind, kit), audio={"A": AudioOptions()})
    error = refused(described(kit, SAI1=periph).machine)
    assert error.key == "board.SAI1.A"
    assert error.expected == "an audio block of an SAI with the built-in model"


@pytest.mark.parametrize("kind", KINDS)
def test_a_pty_on_a_board_model_that_is_no_uart_raises_model_error(kit: Plugin, kind: str) -> None:
    periph = popcorn.Periph(model=model_of(kind, kit), connect="pty")
    with pytest.raises(ModelError):
        described(kit, USART1=periph).machine()


def test_a_plugin_uart_attaches_and_sends(kit: Plugin) -> None:
    m = described(kit, USART2=popcorn.Periph(model=kit.model("uart"))).machine()
    m.periph.USART2.TDR.write(ord("A"))
    port = m.periph.USART2.port
    assert port.take() == b"A"
    asyncio.run(port.write(b"hi"))
    assert [m.periph.USART2.RDR.read() for _ in range(3)] == [ord("h"), ord("i"), RDR_EMPTY]


@pytest.mark.parametrize(("a", "b", "side"), [("USART1", "USART2", "b"), ("USART2", "USART1", "a")])
def test_a_wire_into_a_plugin_uart_is_refused_naming_its_end(
    kit: Plugin, a: str, b: str, side: str
) -> None:
    description = described(kit, USART2=popcorn.Periph(model=kit.model("uart")))
    machines = {name: description.machine(name=name) for name in ("one", "two")}
    link = popcorn.UartLink(f"one.{a}", f"two.{b}", latency=us(20), baud=115_200)
    error = refused(lambda: popcorn.System(machines, [link]))
    assert error.key == f"links[0].{side}"
    receiver = f"two.{b}" if side == "b" else f"one.{a}"
    assert (
        str(error)
        == f"links[0].{side}: {receiver} is not a UART model that takes frames from a wire"
    )
    built_in = popcorn.UartLink("one.USART1", "two.USART1", latency=us(20), baud=115_200)
    popcorn.System(machines, [built_in]).close()


def test_the_digest_follows_the_plugin_and_the_model_name(kit: Plugin) -> None:
    rng = described(kit, RNG=popcorn.Periph(model=kit.model("rng")))
    uart = described(kit, RNG=popcorn.Periph(model=kit.model("uart")))
    assert len({described().digest, described(kit).digest, rng.digest, uart.digest}) == 4


def test_a_description_without_plugins_keeps_its_digest() -> None:
    from popcorn.devices import At25sf

    description = popcorn.MachineDescription(
        part=PART,
        images=[],
        board={
            "TIM2": popcorn.Periph(model=popcorn.Use(Model, rate=3)),
            "SPI1": popcorn.Periph(devices=[popcorn.Device(At25sf, cs="PA4", size=1 << 20)]),
        },
    )
    assert description.digest == BEFORE_PLUGINS_DIGEST


def test_the_digest_holds_the_plugin_code_not_its_path(tmp_path: Path) -> None:
    first = copied("kit", tmp_path / "one")
    second = copied("kit", tmp_path / "two")

    def digest(source: Path) -> str:
        return described(Plugin.dev(source)).digest

    assert digest(first) == digest(second)
    probe = f"""
        import popcorn
        from popcorn.plugin import Plugin
        print(popcorn.MachineDescription(
            part="{PART}", images=[], plugins=[Plugin.dev({str(first)!r})]
        ).digest)
        """
    before = run_python(probe, tmp_path / "cache")
    kit_cpp = first / "kit.cpp"
    kit_cpp.write_text(kit_cpp.read_text().replace("return out + 3;", "return out + 4;"))
    after = run_python(probe, tmp_path / "cache")
    assert before.returncode == 0 and after.returncode == 0, before.stderr + after.stderr
    assert before.stdout.strip() == digest(second)
    assert after.stdout.strip() != before.stdout.strip()


def test_an_installed_plugin_digest_without_bindings_covers_its_library(tmp_path: Path) -> None:
    library = tmp_path / "libkit.so"
    library.write_bytes(b"library one")
    plugin = Plugin("kit", library=library)
    first = described(plugin).digest
    (tmp_path / "_native.cpython-314-x86_64-linux-gnu.so").write_bytes(b"bindings")
    assert described(plugin).digest == first
    library.write_bytes(b"library two")
    third = described(plugin).digest
    moved = tmp_path / "elsewhere"
    moved.mkdir()
    shutil.copy2(library, moved / library.name)
    assert third != first
    assert described(Plugin("kit", library=moved / "libkit.so")).digest == third
    lone = tmp_path / "lone"
    lone.mkdir()
    absent = described(Plugin("kit", library=lone / "libkit.so")).digest
    (lone / "libkit.so").write_bytes(b"")
    assert absent != described(Plugin("kit", library=lone / "libkit.so")).digest


def test_cache_key_takes_a_plugin_model(kit: Plugin) -> None:
    description = described(kit, images=True, RNG=popcorn.Periph(model=kit.model("rng")))
    assert len(cache_key(description, strict=False)) == 64


def test_a_missing_plugin_directory_is_a_missing_input(tmp_path: Path) -> None:
    description = described(Plugin.dev(tmp_path / "absent"))
    with pytest.raises(popcorn.MissingInput) as caught:
        description.machine()
    assert caught.value.key == "plugins[0].source"


def board_machine(kit: Plugin) -> popcorn.Machine:
    m = described(kit, images=True, RNG=popcorn.Periph(model=kit.model("rng"))).machine()
    m.reset()
    stop = asyncio.run(m.run(until=m.sym("ready"), budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS
    return m


def test_the_plugin_model_reads_alike_parked_and_running(kit: Plugin) -> None:
    parked = board_machine(kit)
    running = board_machine(kit)

    async def reads(m: popcorn.Machine) -> list[int]:
        return [int((await m.read(m.ref.periph.RNG.DR.bus))[0]) for _ in range(3)]

    async def while_running() -> list[int]:
        async with running.running():
            return await reads(running)

    assert asyncio.run(reads(parked)) == [RNG_BASE, RNG_BASE + 1, RNG_BASE + 2]
    assert asyncio.run(while_running()) == [RNG_BASE, RNG_BASE + 1, RNG_BASE + 2]


def test_a_machine_with_a_plugin_model_is_collected(kit: Plugin) -> None:
    m = described(kit, RNG=popcorn.Periph(model=kit.model("rng"))).machine()
    assert rng_reads(m, 1) == [RNG_BASE]
    dead = weakref.ref(m)
    del m
    gc.collect()
    assert dead() is None


def collected_with_its_device(m: popcorn.Machine) -> None:
    """Make the SPI1 device of @p m, the only reference to it, reach @p m, then drop both."""
    attached = m.board.devices("SPI1")[0]
    vars(attached)["owner"] = m
    machine, held = weakref.ref(m), weakref.ref(attached)
    del m, attached
    gc.collect()
    assert machine() is None
    assert held() is None


def test_a_plugin_device_that_reaches_its_machine_is_collected(kit: Plugin) -> None:
    device = kit_device(kit)
    board = popcorn.Periph(devices=[popcorn.Device(device, cs="PA4")])
    collected_with_its_device(described(kit, SPI1=board).machine())


def test_a_plugin_device_given_to_machine_that_reaches_it_is_collected(kit: Plugin) -> None:
    device = kit_device(kit)
    collected_with_its_device(
        described(kit).machine(devices={"SPI1": [kit_instance(device, "PA4")]})
    )


def test_a_plugin_device_subclass_alive_at_exit_leaks_nothing(kit: Plugin) -> None:
    ran = run_python(
        f"""
        import popcorn
        from popcorn.devices import chip_select
        from popcorn.plugin import Plugin

        PLUGIN = Plugin.dev({str(FIXTURES / "kit")!r})

        class Device(PLUGIN.module().KitTargetBase):
            def __init__(self):
                super().__init__()
                self._cs_port, self._cs_pin = chip_select("PA4")

        m = popcorn.MachineDescription(
            part="{PART}",
            images=[],
            plugins=[PLUGIN],
            board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(Device)])}},
        ).machine()
        print(type(m.board.devices("SPI1")[0]).__name__)
        """,
        Path(os.environ["POPCORN_PLUGIN_CACHE"]),
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.split() == ["Device"]
    assert "nanobind: leaked" not in ran.stderr


M33_JSON = ROOT / "tests" / "data" / "core" / "m33.json"
DRIVER_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "driver.elf"


def boot_session(root: Path, cache: Path) -> str:
    """The boot cache summary of a pytest session over the tree at @p root."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PYTEST_")}
    environment["POPCORN_PLUGIN_CACHE"] = str(cache)
    ran = subprocess.run(
        [sys.executable, "-m", "pytest", "-p", "no:asyncio", "-p", "no:xdist", "-rA"],
        cwd=root,
        capture_output=True,
        text=True,
        env=environment,
        check=False,
        timeout=600,
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    return next(
        line
        for line in ran.stdout.splitlines()
        if line.startswith("popcorn: ") and "key(s)" in line
    )


@pytest.mark.parametrize(
    "board", ['{"UNMODELLED": popcorn.Periph(model=PLUGIN.model("rng"))}', "{}"]
)
def test_a_plugin_edit_invalidates_the_boot_cache(tmp_path: Path, board: str) -> None:
    source = copied("kit", tmp_path)
    root = tmp_path / "tree"
    root.mkdir()
    (root / "pytest.ini").write_text("[pytest]\n")
    (root / "conftest.py").write_text(textwrap.dedent(f"""
            from pathlib import Path

            import popcorn
            import popcorn.pytest
            from popcorn.plugin import Plugin

            pytest_plugins = ["popcorn.pytest"]

            PLUGIN = Plugin.dev({str(source)!r})
            PROJECT = popcorn.MachineDescription(
                part="POPCORN-M33-TEST",
                description=Path({str(M33_JSON)!r}),
                images=[popcorn.Elf(Path({str(DRIVER_ELF)!r}))],
                boot=popcorn.Boot(until="alu_mark"),
                plugins=[PLUGIN],
                board={board},
            )


            @popcorn.pytest.machine
            def machine():
                return PROJECT
            """))
    (root / "test_boot.py").write_text("def test_boot(machine):\n    pass\n")
    cache = tmp_path / "cache"
    assert boot_session(root, cache) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boot_session(root, cache) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    kit_cpp = source / "kit.cpp"
    kit_cpp.write_text(kit_cpp.read_text().replace("return out + 3;", "return out + 4;"))
    assert boot_session(root, cache) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"

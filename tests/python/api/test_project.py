"""Projects: the Python spec, its digest, project files, configs, FR-CFG-4 errors, machine
building and boot."""

from __future__ import annotations

import asyncio
import copy
import importlib
import os
import subprocess
import sys
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any, Literal, cast

import popcorn
import pytest
from popcorn import _core
from popcorn.board import (
    AudioOptions,
    I2CTarget,
    RccOptions,
    SpiTarget,
    SupplyOptions,
    UsbHostOptions,
)
from popcorn.devices import At25sf, Eeprom24xx
from popcorn.model import Model
from popcorn.stm32.description import DescriptionError
from popcorn.units import ms, us

from .conftest import DRIVER_ELF, M33_JSON, ROOT

DATA = ROOT / "tests" / "data" / "api"


def write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "popcorn.toml"
    path.write_text(text)
    return path


def minimal_project(
    *,
    vtor: int | None = None,
    budget: int | None = None,
    memory_init: Sequence[popcorn.MemoryInit] = (),
    arena_base: int | Literal[False] | None = None,
    arena_size: int | None = None,
) -> popcorn.Project:
    return popcorn.Project(
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF)],
        boot=popcorn.Boot(
            vtor=vtor,
            until="alu_mark",
            budget=budget,
            memory_init=memory_init,
            arena_base=arena_base,
            arena_size=arena_size,
        ),
    )


def minimal(**boot: str) -> str:
    extra = "".join(f"{key} = {value}\n" for key, value in boot.items())
    return (
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{DRIVER_ELF}"\n'
        f'[boot]\nuntil = "alu_mark"\n{extra}'
    )


def test_the_full_project_reads_back() -> None:
    project = popcorn.Project.load(DATA / "full.toml")
    assert project.name == "demo"
    assert project.configs == {}
    assert [type(image) for image in project.images] == [popcorn.Elf, popcorn.Bin]
    board = project.board
    assert board["UART4"].connect == "pty"
    device = board["I2C1"].devices[0]
    assert (device.address, device.cls.__name__, device.params) == (
        0x50,
        "Eeprom",
        {"size": 8192},
    )
    model = board["TIM2"].model
    assert model is not None and model.cls.__name__ == "Timer"
    assert model.params == {"prescaler": 4}


def test_a_config_overrides_keys() -> None:
    project = popcorn.Project.load(DATA / "full.toml", "variant")
    assert project.images[0].path.name == "basic.elf"
    assert project.digest != popcorn.Project.load(DATA / "full.toml").digest


def test_an_unknown_config_key_names_the_config() -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(DATA / "full.toml", "broken")
    assert caught.value.key == "configs.broken.image.app.elff"
    assert caught.value.file == str(DATA / "full.toml")
    assert caught.value.line == 47


@pytest.mark.parametrize(
    ("text", "key", "line"),
    [
        ('[mcu]\npart = "X"\nspeed = 1\n', "mcu.speed", 3),
        ("[mcu]\npart = 7\n", "mcu.part", 2),
        ('[mcu]\nioc = "a.ioc"\n', "mcu.part", 1),
    ],
    ids=["unknown key", "wrong type", "missing part"],
)
def test_invalid_files_name_file_line_and_key(
    tmp_path: Path, text: str, key: str, line: int
) -> None:
    path = write(tmp_path, text)
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(path)
    assert (caught.value.file, caught.value.line, caught.value.key) == (str(path), line, key)


def test_an_unknown_part_lists_close_matches() -> None:
    project = popcorn.Project(part="STM32U575QGI", images=[])
    try:
        project.machine_description()
    except popcorn.ConfigError as error:
        if "close matches" not in error.expected:
            pytest.skip(f"ST's description sources are not installed: {error}")
        assert "STM32U575QGIx" in error.expected
        assert isinstance(error, DescriptionError)
    else:
        pytest.fail("an unknown part built a description")


@pytest.mark.parametrize(
    ("model", "expected"),
    [
        ("absent_module:Eeprom", "an importable module"),
        ("fake_models:Absent", "a class of fake_models"),
        ("fake_models:NOT_A_CLASS", "a class, fake_models:NOT_A_CLASS is a int"),
        ("fake_models", "module:Class"),
    ],
)
def test_bad_model_references_name_the_key(tmp_path: Path, model: str, expected: str) -> None:
    text = (
        f'[mcu]\npart = "X"\n[models]\npython = ["{DATA / "models"}"]\n'
        f'[board.TIM2]\nmodel = "{model}"\n'
    )
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, text))
    assert caught.value.key == "board.TIM2.model"
    assert caught.value.expected.startswith(expected)


def test_a_description_for_another_part_is_refused() -> None:
    project = popcorn.Project(part="OTHER", description=M33_JSON, images=[])
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine_description()
    assert caught.value.key == "description"


def test_the_minimal_project_boots_to_its_symbol() -> None:
    project = minimal_project(budget=1_000_000)
    m = project.machine()
    assert m.config_digest == project.digest
    stop = asyncio.run(project.boot_machine(m))
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert stop.location.symbol == "alu_mark"


def test_boot_raises_fault_error_on_a_fault() -> None:
    table = popcorn.Image.elf(DRIVER_ELF).lookup("vt_fault").value
    project = minimal_project(vtor=table)
    with pytest.raises(popcorn.FaultError) as caught:
        asyncio.run(project.boot_machine(project.machine()))
    assert caught.value.stop is not None
    assert caught.value.stop.reason is popcorn.StopReason.FAULT


def test_boot_raises_budget_exceeded_before_its_symbol() -> None:
    project = minimal_project(budget=100)
    with pytest.raises(popcorn.BudgetExceeded) as caught:
        asyncio.run(project.boot_machine(project.machine()))
    assert caught.value.budget == 100
    assert caught.value.stop is not None


def test_memory_init_needs_a_known_symbol() -> None:
    project = minimal_project(memory_init=[popcorn.MemoryInit("absent", b"\x01")])
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == "boot.memory_init[0].symbol"


def test_memory_init_writes_before_boot() -> None:
    minimal_project(memory_init=[popcorn.MemoryInit("spin_flag", b"\x01\x00\x00\x00")]).machine()


def test_the_digest_follows_values_not_layout(tmp_path: Path) -> None:
    a = tmp_path / "a.toml"
    b = tmp_path / "b.toml"
    c = tmp_path / "c.toml"
    a.write_text('[mcu]\npart = "X"\n[board.UART4]\nconnect = "pty"\n')
    b.write_text('# layout only\n[mcu]\n  part="X"\n\n[board.UART4]\nconnect   =  "pty"\n')
    c.write_text('[mcu]\npart = "X"\n[board.UART4]\nconnect = "tcp"\n')
    digest = [popcorn.Project.load(path).digest for path in (a, b, c)]
    assert digest[0] == digest[1] != digest[2]


def test_machine_with_a_config_reloads_the_file(tmp_path: Path) -> None:
    text = (
        minimal() + '\n[configs.same]\nboot.until = "alu_mark"\n[configs.slow]\nclock.cpi = 2.0\n'
    )
    path = write(tmp_path, text)
    project = popcorn.Project.load(path)
    assert popcorn.Project.load(path, "same").machine().config_digest == project.digest
    assert popcorn.Project.load(path, "slow").machine().config_digest != project.digest


def test_description_error_is_a_config_error() -> None:
    error = DescriptionError("part.svd", 4, "cpu.name", "a core name")
    assert isinstance(error, popcorn.ConfigError)
    assert (error.file, error.line, error.key, error.expected) == (
        "part.svd",
        4,
        "cpu.name",
        "a core name",
    )


def test_the_description_package_imports_without_the_native_extension() -> None:
    probe = (
        "import sys, types\n"
        "package = types.ModuleType('popcorn')\n"
        "package.__path__ = [sys.argv[1]]\n"
        "sys.modules['popcorn'] = package\n"
        "from popcorn.stm32.description import DescriptionError\n"
        "from popcorn._errors import ConfigError\n"
        "assert issubclass(DescriptionError, ConfigError)\n"
        "assert 'popcorn._core' not in sys.modules\n"
        "print('ok')\n"
    )
    done = subprocess.run(
        [sys.executable, "-c", probe, str(ROOT / "python" / "popcorn")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert done.stdout.strip() == "ok", done.stderr


def test_python_side_errors_carry_the_line(tmp_path: Path) -> None:
    text = f'[mcu]\npart = "OTHER"\ndescription = "{M33_JSON}"\n'
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, text)).machine_description()
    assert (caught.value.key, caught.value.line) == ("mcu.description", 3)


def test_memory_writes_stay_in_the_32_bit_address_space(m33: popcorn.MachineDescription) -> None:
    handle = _core.MachineHandle(m33, _core.MachineConfig())
    with pytest.raises(popcorn.ConfigError) as caught:
        handle.write_memory(0xFFFF_FFFF, b"\x00\x00")
    assert caught.value.key == "0xffffffff"
    assert "32-bit address space" in caught.value.expected
    with pytest.raises(popcorn.EmulationError, match="0x30000000"):
        handle.write_memory(0x3000_0000, b"\x00")


def test_boot_arena_keys_set_or_disable_the_arena() -> None:
    moved = minimal_project(arena_base=0x6E00_0000, arena_size=0x2000).machine()
    assert (moved.arena.base, moved.arena.size) == (0x6E00_0000, 0x2000)
    none = minimal_project(arena_base=False).machine()
    with pytest.raises(popcorn.ConfigError, match="no arena"):
        _ = none.arena
    default = minimal_project().machine()
    assert default.arena.base == 0x6F00_0000


def test_a_loaded_arena_base_of_true_is_refused_on_its_line(tmp_path: Path) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, minimal(arena_base="true")))
    assert (caught.value.key, caught.value.line) == ("boot.arena_base", 8)


class Target(I2CTarget):
    def __init__(self, address: int, size: int = 16) -> None:
        super().__init__(address)
        self.size = size


class Flash(SpiTarget):
    def __init__(self, address: int) -> None:
        super().__init__()


class NoCs(SpiTarget):
    def __init__(self, size: int) -> None:
        super().__init__()


class Loose(SpiTarget):
    def __init__(self, **options: object) -> None:
        super().__init__(cast("str | None", options.get("cs")))


class NotADevice:
    def __init__(self, address: int) -> None:
        self.address = address


def with_devices(*devices: popcorn.Device) -> popcorn.Project:
    return minimal_project().replace(board={"TESTDEV": popcorn.Periph(devices=devices)})


def test_a_device_class_must_be_a_bus_target() -> None:
    project = with_devices(popcorn.Device(NotADevice, address=0x50))
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == "board.TESTDEV.devices[0].model"
    assert "I2CTarget or SpiTarget" in caught.value.expected


def test_devices_need_an_i2c_bus() -> None:
    project = with_devices(popcorn.Device(Target, address=0x50))
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == "board.TESTDEV.devices[0].model"
    assert caught.value.expected == "an I2C bus of POPCORN-M33-TEST"


def test_an_spi_class_used_with_cs_must_take_a_cs_keyword() -> None:
    project = with_devices(popcorn.Device(NoCs, cs="PA4", size=1))
    with pytest.raises(popcorn.ConfigError) as caught:
        project.machine()
    assert caught.value.key == "board.TESTDEV.devices[0].cs"
    assert caught.value.expected == "a cs keyword accepted by NoCs"


def test_an_spi_class_taking_any_keyword_takes_cs() -> None:
    device = with_devices(popcorn.Device(Loose, cs="PA4", size=1)).board["TESTDEV"].devices[0]
    assert (device.cs, device.params) == ("PA4", {"size": 1})


def test_the_full_project_equals_its_python_spec() -> None:
    loaded = popcorn.Project.load(DATA / "full.toml")
    models = importlib.import_module("fake_models")
    eeprom = cast(type, models.Eeprom)
    timer = cast(type, models.Timer)
    spec = popcorn.Project(
        name="demo",
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF), popcorn.Bin(DATA / "blob.bin", 0x2000_0100)],
        boot=popcorn.Boot(
            vtor=0x0800_0000,
            until="alu_mark",
            budget=1_000_000,
            memory_init=[popcorn.MemoryInit("spin_flag", bytes([1, 0, 0, 0]))],
        ),
        core_hz=8_000_000,
        board={
            "UART4": popcorn.Periph(connect="pty"),
            "I2C1": popcorn.Periph(devices=[popcorn.Device(eeprom, address=0x50, size=8192)]),
            "TIM2": popcorn.Periph(model=popcorn.Use(timer, prescaler=4)),
        },
        strict=True,
    )
    assert loaded == spec
    assert loaded.digest == spec.digest
    assert hash(loaded) == hash(spec)


def sample() -> popcorn.Project:
    """A project using every kind of field and parameter value."""
    return popcorn.Project(
        name="sample",
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF), popcorn.Bin(DATA / "blob.bin", 0x2000_0100)],
        boot=popcorn.Boot(
            until="alu_mark",
            budget=1_000_000,
            memory_init=[popcorn.MemoryInit("spin_flag", b"\x01\x00")],
        ),
        cpi=1.5,
        board={
            "SPI1": popcorn.Periph(
                devices=[
                    popcorn.Device(
                        At25sf,
                        cs="PA4",
                        jedec=[0x1F, 0x86, 0x01],
                        page_program=us(600),
                        table={0x0F: 1, 3: [b"\x02", None, True, 0.25]},
                    )
                ]
            ),
            "I2C1": popcorn.Periph(devices=[popcorn.Device(Eeprom24xx, address=0x50, size=256)]),
            "TIM2": popcorn.Periph(model=popcorn.Use(Model, data=M33_JSON, mode={"b": 1, "a": 2})),
            "SAI1": popcorn.Periph(audio={"A": AudioOptions(decimation=2)}),
            "USB_OTG_FS": popcorn.Periph(connect="pty", host=UsbHostOptions(reset=ms(20))),
        },
    )


def test_the_digest_is_equal_across_processes() -> None:
    probe = (
        "import sys\n"
        "sys.path.insert(0, sys.argv[1])\n"
        "from api.test_project import sample\n"
        "print(sample().digest)\n"
    )
    digests: set[str] = set()
    for seed in ("1", "2"):
        done = subprocess.run(
            [sys.executable, "-c", probe, str(ROOT / "tests" / "python")],
            capture_output=True,
            text=True,
            check=False,
            cwd=Path.cwd(),
            env={**os.environ, "PYTHONHASHSEED": seed},
        )
        assert done.returncode == 0, done.stderr
        digests.add(done.stdout.strip())
    assert digests == {sample().digest}


def _periph(name: str, periph: popcorn.Periph) -> Callable[[popcorn.Project], popcorn.Project]:
    return lambda p: p.replace(board={**p.board, name: periph})


def _boot(**changes: object) -> Callable[[popcorn.Project], popcorn.Project]:
    return lambda p: p.replace(boot=p.boot.replace(**changes))


def _device(**params: popcorn.Param) -> popcorn.Periph:
    return popcorn.Periph(
        devices=[popcorn.Device(At25sf, cs="PA4", **cast(dict[str, Any], params))]
    )


DIGEST_CHANGES: dict[str, Callable[[popcorn.Project], popcorn.Project]] = {
    "part": lambda p: p.replace(part="OTHER"),
    "image path": lambda p: p.replace(images=[popcorn.Elf(DRIVER_ELF.with_name("hooks.elf"))]),
    "image kind": lambda p: p.replace(images=[popcorn.Hex(DRIVER_ELF), *p.images[1:]]),
    "image order": lambda p: p.replace(images=list(reversed(p.images))),
    "bin path": lambda p: p.replace(
        images=[p.images[0], popcorn.Bin(DATA / "other.bin", 0x2000_0100)]
    ),
    "bin address": lambda p: p.replace(
        images=[p.images[0], popcorn.Bin(DATA / "blob.bin", 0x2000_0200)]
    ),
    "core": lambda p: p.replace(core="cm4"),
    "svd": lambda p: p.replace(svd=DATA / "part.svd"),
    "description": lambda p: p.replace(description=None),
    "boot vtor": _boot(vtor=0x0800_0000),
    "boot until": _boot(until="main"),
    "boot budget": _boot(budget=2_000_000),
    "boot memory_init": _boot(memory_init=[popcorn.MemoryInit("spin_flag", b"\x01\x01")]),
    "boot memory_init symbol": _boot(memory_init=[popcorn.MemoryInit("alu_rounds", b"\x01\x00")]),
    "boot arena_base": _boot(arena_base=False),
    "boot arena_size": _boot(arena_size=0x2000),
    "core_hz": lambda p: p.replace(core_hz=8_000_000),
    "cpi": lambda p: p.replace(cpi=2.0),
    "rcc": lambda p: p.replace(rcc=RccOptions(hse_hz=8_000_000)),
    "supply": lambda p: p.replace(supply=SupplyOptions(vdda_mv=3300)),
    "peripheral name": lambda p: p.replace(
        board={("SPI2" if k == "SPI1" else k): v for k, v in p.board.items()}
    ),
    "model class": _periph("TIM2", popcorn.Periph(model=popcorn.Use(At25sf))),
    "model param": _periph(
        "TIM2", popcorn.Periph(model=popcorn.Use(Model, data=M33_JSON, mode={"b": 1, "a": 3}))
    ),
    "path param": _periph(
        "TIM2", popcorn.Periph(model=popcorn.Use(Model, data=DRIVER_ELF, mode={"b": 1, "a": 2}))
    ),
    "device address": _periph(
        "I2C1", popcorn.Periph(devices=[popcorn.Device(Eeprom24xx, address=0x51, size=256)])
    ),
    "device cs removed": _periph("SPI1", popcorn.Periph(devices=[popcorn.Device(At25sf)])),
    "device cs": _periph("SPI1", popcorn.Periph(devices=[popcorn.Device(At25sf, cs="PA5")])),
    "device param": _periph("SPI1", _device(jedec=[0x1F, 0x86, 0x02])),
    "duration param": _periph("SPI1", _device(page_program=us(601))),
    "bytes param": _periph("SPI1", _device(table={3: [b"\x03"]})),
    "connect": _periph("USB_OTG_FS", popcorn.Periph(host=UsbHostOptions(reset=ms(20)))),
    "audio": _periph("SAI1", popcorn.Periph(audio={"B": AudioOptions(decimation=2)})),
    "host": _periph("USB_OTG_FS", popcorn.Periph(connect="pty", host=UsbHostOptions(reset=ms(30)))),
}


@pytest.mark.parametrize("change", DIGEST_CHANGES.values(), ids=DIGEST_CHANGES.keys())
def test_the_digest_follows_every_field(
    change: Callable[[popcorn.Project], popcorn.Project],
) -> None:
    base = sample()
    changed = change(base)
    assert changed != base
    assert changed.digest != base.digest


@pytest.mark.parametrize(
    ("first", "second"), [(1, 1.0), (True, 1), (False, 0)], ids=["int float", "bool int", "false"]
)
def test_the_digest_tells_apart_params_python_calls_equal(
    first: popcorn.Param, second: popcorn.Param
) -> None:
    uses = (popcorn.Use(Model, rate=first), popcorn.Use(Model, rate=second))
    assert uses[0] == uses[1]
    project = sample()
    digests = {project.replace(board={"TIM2": popcorn.Periph(model=u)}).digest for u in uses}
    assert len(digests) == 2


def test_the_digest_ignores_key_order() -> None:
    base = sample()
    reordered_board = dict(reversed(list(base.board.items())))
    tim2 = popcorn.Periph(model=popcorn.Use(Model, mode={"a": 2, "b": 1}, data=M33_JSON))
    changed = base.replace(board={**reordered_board, "TIM2": tim2})
    assert list(changed.board) != list(base.board)
    assert changed.digest == base.digest


def test_use_and_device_replace_one_param() -> None:
    use = popcorn.Use(Model, rate=1, mode="a")
    assert use.replace(rate=2) == popcorn.Use(Model, rate=2, mode="a")
    assert copy.replace(use, mode="b") == popcorn.Use(Model, rate=1, mode="b")
    device = popcorn.Device(Eeprom24xx, address=0x50, size=256)
    assert device.replace(size=512) == popcorn.Device(Eeprom24xx, address=0x50, size=512)
    assert copy.replace(device, address=0x51) == popcorn.Device(Eeprom24xx, address=0x51, size=256)
    spi = popcorn.Device(At25sf, cs="PA4", size=1 << 20)
    assert spi.replace(cs=None).cs is None
    assert spi.replace(size=2 << 20).cs == "PA4"
    with pytest.raises(popcorn.ConfigError) as caught:
        device.replace(address="0x51")
    assert caught.value.key == "address"
    with pytest.raises(popcorn.ConfigError):
        use.replace(rate=float("nan"))


def test_each_machine_model_gets_its_own_params() -> None:
    made: list[list[int]] = []

    class Keeper(Model):
        def __init__(self, items: list[int]) -> None:
            super().__init__()
            made.append(items)

    use = popcorn.Use(Keeper, items=[1, 2])
    project = popcorn.Project(
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF)],
        board={"TESTDEV": popcorn.Periph(model=use)},
    )
    project.machine()
    project.machine()
    assert made == [[1, 2], [1, 2]]
    assert made[0] is not made[1]
    made[0].append(3)
    assert use.params["items"] == (1, 2)


def test_the_digest_leaves_out_name_strict_configs_and_source() -> None:
    base = sample()
    changed = base.replace(name="other", strict=True).with_configs(fast=base.replace(cpi=1.0))
    assert changed != base
    assert changed.digest == base.digest
    loaded = popcorn.Project.load(DATA / "full.toml")
    assert loaded.source is not None
    assert loaded.replace(source=None) == loaded


@pytest.mark.parametrize(
    "value",
    [
        lambda: 0,
        open,
        object(),
        {1, 2},
        {"a": 1, 2: 3},
        bytearray(b"x"),
        float("nan"),
        float("inf"),
        [float("-inf")],
    ],
    ids=["lambda", "function", "object", "set", "mixed keys", "bytearray", "nan", "inf", "-inf"],
)
def test_a_param_that_is_not_data_is_refused(value: object) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Use(Model, value=cast(popcorn.Param, value))
    assert caught.value.key.startswith("value")


def test_an_open_file_param_is_refused(tmp_path: Path) -> None:
    with (tmp_path / "f.bin").open("wb") as file, pytest.raises(popcorn.ConfigError):
        popcorn.Device(At25sf, image=cast(popcorn.Param, file))


def test_params_are_frozen_and_thawed_for_each_machine() -> None:
    jedec = [0x1F, 0x86, 0x01]
    device = popcorn.Device(At25sf, jedec=jedec, mode={"a": [1]})
    jedec.append(0)
    assert device.params == {"jedec": (0x1F, 0x86, 0x01), "mode": {"a": (1,)}}
    assert hash(device) == hash(popcorn.Device(At25sf, jedec=(0x1F, 0x86, 0x01), mode={"a": [1]}))
    assert popcorn.Use(Model, data=Path("x")).params["data"] == Path("x")


def test_a_project_is_hashable_and_immutable() -> None:
    project = sample()
    assert {project: 1}[sample()] == 1
    with pytest.raises(AttributeError):
        cast(Any, project).part = "OTHER"
    with pytest.raises(TypeError):
        cast(dict[str, popcorn.Periph], project.board)["X"] = popcorn.Periph()


def test_configs_are_named_variants() -> None:
    base = sample()
    fast = base.replace(cpi=1.0)
    project = base.with_configs(fast=fast, slow=base.replace(cpi=3.0))
    assert project.config(None) is project
    assert project.config("fast") == fast
    assert sorted(project.configs) == ["fast", "slow"]
    with pytest.raises(popcorn.ConfigError) as caught:
        project.config("absent")
    assert caught.value.key == "configs.absent"
    assert caught.value.expected == "a config of the project: fast, slow"


@pytest.mark.parametrize(
    ("make", "key"),
    [
        (lambda: popcorn.Boot(budget=-1), "boot.budget"),
        (lambda: popcorn.Boot(arena_base=0x1_0000_0000), "boot.arena_base"),
        (lambda: popcorn.Boot(arena_size=0), "boot.arena_size"),
        (lambda: popcorn.Boot(vtor=-4), "boot.vtor"),
        (lambda: popcorn.Project(part="X", images=[], cpi=0), "cpi"),
        (lambda: popcorn.Project(part="X", images=[], core_hz=0), "core_hz"),
        (
            lambda: popcorn.Project(
                part="X",
                images=[],
                board={"I2C1": popcorn.Periph(devices=[popcorn.Device(At25sf, address=0x78)])},
            ),
            "board.I2C1.devices[0].address",
        ),
        (
            lambda: popcorn.Project(
                part="X",
                images=[],
                board={
                    "I2C1": popcorn.Periph(devices=[popcorn.Device(At25sf, address=0x50, cs="PA4")])
                },
            ),
            "board.I2C1.devices[0].cs",
        ),
        (
            lambda: popcorn.Project(
                part="X",
                images=[],
                board={
                    "TIM2": popcorn.Periph(
                        model=popcorn.Use(Model), devices=[popcorn.Device(At25sf)]
                    )
                },
            ),
            "board.TIM2.devices",
        ),
        (
            lambda: popcorn.Project(part="X", images=cast(list[popcorn.ImageFile], ["app.elf"])),
            "images[0]",
        ),
        (
            lambda: popcorn.Project(
                part="X", images=[], board=cast(dict[str, popcorn.Periph], {"TIM2": Model})
            ),
            "board.TIM2",
        ),
        (
            lambda: popcorn.Project(
                part="X", images=[], board={"TIM2": popcorn.Periph(model=cast(popcorn.Use, Model))}
            ),
            "board.TIM2.model",
        ),
        (
            lambda: popcorn.Project(
                part="X",
                images=[],
                board={
                    "I2C1": popcorn.Periph(
                        devices=cast(list[popcorn.Device], [popcorn.Use(Eeprom24xx)])
                    )
                },
            ),
            "board.I2C1.devices[0]",
        ),
        (
            lambda: popcorn.Project(
                part="X", images=[], configs=cast(dict[str, popcorn.Project], {"x": "y"})
            ),
            "configs.x",
        ),
        (lambda: popcorn.MemoryInit("flag", cast(bytes, "01")), "data"),
    ],
    ids=[
        "negative budget",
        "arena",
        "arena size",
        "vtor",
        "cpi",
        "core_hz",
        "address",
        "address and cs",
        "both",
        "str image",
        "board value",
        "model",
        "device",
        "config",
        "memory_init data",
    ],
)
def test_construction_refuses_what_types_cannot_say(make: Callable[[], object], key: str) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        make()
    assert (caught.value.key, caught.value.line, caught.value.file) == (key, 0, "")


def test_a_python_project_boots_and_checks_against_its_description() -> None:
    project = popcorn.Project(
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF)],
        boot=popcorn.Boot(until="alu_mark", budget=1_000_000),
    )
    m = project.machine()
    assert m.config_digest == project.digest
    stop = asyncio.run(project.boot_machine(m))
    assert stop.location.symbol == "alu_mark"
    with pytest.raises(popcorn.ConfigError) as caught:
        project.replace(part="OTHER").machine_description()
    assert caught.value.key == "description"
    unknown = project.replace(board={"TIM9": popcorn.Periph(model=popcorn.Use(Model))})
    with pytest.raises(popcorn.ConfigError) as caught:
        unknown.machine()
    assert (caught.value.key, caught.value.line) == ("board.TIM9", 0)


@pytest.mark.parametrize(
    ("make", "key"),
    [
        (
            lambda: popcorn.Project(part="X", images=[popcorn.Elf(Path("app.elf"))]),
            "images[0].path",
        ),
        (
            lambda: popcorn.Project(
                part="X", images=[popcorn.Elf(DRIVER_ELF), popcorn.Bin(Path("b.bin"), 0)]
            ),
            "images[1].path",
        ),
        (lambda: popcorn.Project(part="X", images=[popcorn.Hex(Path("a.hex"))]), "images[0].path"),
        (lambda: popcorn.Project(part="X", images=[], svd=Path("part.svd")), "svd"),
        (lambda: popcorn.Project(part="X", images=[], description=Path("m.json")), "description"),
        (
            lambda: popcorn.Project(part="X", images=[popcorn.Elf(cast(Path, str(DRIVER_ELF)))]),
            "images[0].path",
        ),
    ],
    ids=["elf", "bin", "hex", "svd", "description", "str"],
)
def test_a_relative_path_is_refused(make: Callable[[], object], key: str) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        make()
    assert caught.value.key == key


def test_paths_are_kept_as_given() -> None:
    linked = DRIVER_ELF.parent / ".." / DRIVER_ELF.parent.name / DRIVER_ELF.name
    project = popcorn.Project(part="X", images=[popcorn.Elf(linked)])
    assert project.images[0].path is linked
    assert project.digest == popcorn.Project(part="X", images=[popcorn.Elf(linked)]).digest
    assert "Path(__file__).parent" in _refused_message()


def _refused_message() -> str:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project(part="X", images=[popcorn.Elf(Path("app.elf"))])
    return caught.value.expected


def test_cpi_digest_is_the_same_for_an_int_and_a_float() -> None:
    whole = sample().replace(cpi=2)
    assert whole.cpi == 2 and isinstance(whole.cpi, int)
    assert whole.digest == sample().replace(cpi=2.0).digest


@pytest.mark.parametrize(
    ("periph", "key"),
    [
        (popcorn.Periph(model=popcorn.Use(Model, data=Path("t.bin"))), "board.TESTDEV.model.data"),
        (
            popcorn.Periph(model=popcorn.Use(Model, files=[DATA, Path("t.bin")])),
            "board.TESTDEV.model.files[1]",
        ),
        (
            popcorn.Periph(model=popcorn.Use(Model, files={"a": {"b": Path("t.bin")}})),
            "board.TESTDEV.model.files.a.b",
        ),
        (
            popcorn.Periph(
                devices=[popcorn.Device(At25sf), popcorn.Device(At25sf, image=Path("i"))]
            ),
            "board.TESTDEV.devices[1].image",
        ),
    ],
    ids=["model", "list", "nested mapping", "device"],
)
def test_a_relative_path_param_is_refused(periph: popcorn.Periph, key: str) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project(part="X", images=[], board={"TESTDEV": periph})
    assert caught.value.key == key
    assert "Path(__file__).parent" in caught.value.expected


def test_an_absolute_path_param_is_kept_as_given() -> None:
    linked = DATA / ".." / "api" / "blob.bin"
    use = popcorn.Use(Model, data=linked)
    project = popcorn.Project(part="X", images=[], board={"TESTDEV": popcorn.Periph(model=use)})
    model = project.board["TESTDEV"].model
    assert model is not None and model.params["data"] is linked

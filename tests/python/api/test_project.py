"""Project files: loading, configs, FR-CFG-4 errors, machine building and boot."""

from __future__ import annotations

import asyncio
import subprocess
import sys
from pathlib import Path

import popcorn
import pytest
from popcorn import _core
from popcorn.stm32.description import DescriptionError

from .conftest import DRIVER_ELF, M33_JSON, ROOT

DATA = ROOT / "tests" / "data" / "api"


def write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "popcorn.toml"
    path.write_text(text)
    return path


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
    assert project.configs == ("broken", "variant")
    assert project.config is None
    assert project.file.image_order == ["app", "boot"]
    board = project.board()
    assert board["UART4"].connect == "pty"
    device = board["I2C1"].devices[0]
    assert (device.address, device.model.__name__, device.params) == (
        0x50,
        "Eeprom",
        {"size": 8192},
    )
    assert board["TIM2"].model is not None and board["TIM2"].model.__name__ == "Timer"
    assert board["TIM2"].params == {"prescaler": 4}


def test_a_config_overrides_keys() -> None:
    project = popcorn.Project.load(DATA / "full.toml", "variant")
    assert project.config == "variant"
    assert project.file.mcu.ioc is not None and project.file.mcu.ioc.endswith("variant.ioc")
    assert project.file.image["app"].elf is not None
    assert project.file.image["app"].elf.endswith("basic.elf")
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


def test_an_unknown_part_lists_close_matches(tmp_path: Path) -> None:
    project = popcorn.Project.load(write(tmp_path, '[mcu]\npart = "STM32U575QGI"\n'))
    try:
        project.description()
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
    project = popcorn.Project.load(write(tmp_path, text))
    with pytest.raises(popcorn.ConfigError) as caught:
        project.board()
    assert caught.value.key == "board.TIM2.model"
    assert caught.value.expected.startswith(expected)


def test_a_description_for_another_part_is_refused(tmp_path: Path) -> None:
    text = f'[mcu]\npart = "OTHER"\ndescription = "{M33_JSON}"\n'
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, text)).description()
    assert caught.value.key == "mcu.description"


def test_the_minimal_project_boots_to_its_symbol(tmp_path: Path) -> None:
    project = popcorn.Project.load(write(tmp_path, minimal(budget="1_000_000")))
    m = project.machine()
    assert m.config_digest == project.digest
    stop = asyncio.run(project.boot(m))
    assert stop.reason is popcorn.StopReason.ADDRESS
    assert stop.location.symbol == "alu_mark"


def test_boot_raises_fault_error_on_a_fault(tmp_path: Path) -> None:
    table = popcorn.Image.elf(DRIVER_ELF).lookup("vt_fault").value
    project = popcorn.Project.load(write(tmp_path, minimal(vtor=hex(table))))
    with pytest.raises(popcorn.FaultError) as caught:
        asyncio.run(project.boot(project.machine()))
    assert caught.value.stop is not None
    assert caught.value.stop.reason is popcorn.StopReason.FAULT


def test_boot_raises_budget_exceeded_before_its_symbol(tmp_path: Path) -> None:
    project = popcorn.Project.load(write(tmp_path, minimal(budget="100")))
    with pytest.raises(popcorn.BudgetExceeded) as caught:
        asyncio.run(project.boot(project.machine()))
    assert caught.value.budget == 100
    assert caught.value.stop is not None


def test_memory_init_needs_a_known_symbol(tmp_path: Path) -> None:
    text = minimal() + '\nmemory_init = [{ symbol = "absent", bytes = "01" }]\n'
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, text)).machine()
    assert caught.value.key == "boot.memory_init[0].symbol"


def test_memory_init_writes_before_boot(tmp_path: Path) -> None:
    text = minimal() + '\nmemory_init = [{ symbol = "spin_flag", bytes = "01000000" }]\n'
    popcorn.Project.load(write(tmp_path, text)).machine()


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
    project = popcorn.Project.load(write(tmp_path, text))
    assert project.machine("same").config_digest == project.digest
    assert project.machine("slow").config_digest != project.digest


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
        popcorn.Project.load(write(tmp_path, text)).description()
    assert (caught.value.key, caught.value.line) == ("mcu.description", 3)


def test_memory_writes_stay_in_the_32_bit_address_space(m33: popcorn.MachineDescription) -> None:
    handle = _core.MachineHandle(m33, _core.MachineConfig())
    with pytest.raises(popcorn.ConfigError) as caught:
        handle.write_memory(0xFFFF_FFFF, b"\x00\x00")
    assert caught.value.key == "0xffffffff"
    assert "32-bit address space" in caught.value.expected
    with pytest.raises(popcorn.EmulationError, match="0x30000000"):
        handle.write_memory(0x3000_0000, b"\x00")


def test_boot_arena_keys_set_or_disable_the_arena(tmp_path: Path) -> None:
    moved = popcorn.Project.load(
        write(tmp_path, minimal(arena_base="0x6E00_0000", arena_size="0x2000"))
    ).machine()
    assert (moved.arena.base, moved.arena.size) == (0x6E00_0000, 0x2000)
    none = popcorn.Project.load(write(tmp_path, minimal(arena_base="false"))).machine()
    with pytest.raises(popcorn.ConfigError, match="no arena"):
        _ = none.arena
    default = popcorn.Project.load(write(tmp_path, minimal())).machine()
    assert default.arena.base == 0x6F00_0000
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(write(tmp_path, minimal(arena_base="true")))
    assert (caught.value.key, caught.value.line) == ("boot.arena_base", 8)

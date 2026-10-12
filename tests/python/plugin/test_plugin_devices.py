"""`machine_factory(devices=)`: bus devices given by the test on the project build, and the
boots they share."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import M33_JSON, ROOT, write_conftest, write_project_conftest

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")
I2C_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "i2c_ll.elf"
API = ROOT / "tests" / "python"


@pytest.fixture(scope="module")
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


@pytest.fixture
def u575_project(u575_json: Path, pytester: pytest.Pytester) -> Path:
    write_conftest(pytester)
    return write_project_conftest(
        pytester.path,
        "popcorn.Project(\n"
        '    part="STM32U575QGIx",\n'
        f"    description=Path({str(u575_json)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(I2C_ELF)!r}))],\n"
        '    boot=popcorn.Boot(until="main", budget=100_000_000),\n'
        "    board={\n"
        '        "I2C1": popcorn.Periph(\n'
        "            devices=[popcorn.Device(Eeprom24, address=0x50, size=4096)]\n"
        "        )\n"
        "    },\n"
        ")",
        f"import sys\nsys.path.insert(0, {str(API)!r})\nfrom api.devices_example import Eeprom24",
    )


def summary(result: pytest.RunResult) -> str:
    return next(
        line for line in result.outlines if line.startswith("popcorn: ") and "key(s)" in line
    )


KEYED = """
from api.devices_example import Eeprom24


def test_one_boot_per_device_set(machine_factory):
    given = Eeprom24(0x50, 4096)
    first = machine_factory(devices={"I2C1": [given]}, name="first")
    assert first.name == "first"
    assert first.board.devices("I2C1") == (given,)
    assert first.pc == first.sym("main")
    again = Eeprom24(0x50, 4096)
    second = machine_factory(devices={"SEC_I2C1": [again]}, name="second")
    assert second.name == "second"
    assert second.board.devices("I2C1") == (again,)
    assert second.pc == first.pc and second.instructions == first.instructions
    small = machine_factory(devices={"I2C1": [Eeprom24(0x50, 256)]})
    assert small.pc == first.pc


async def test_the_async_form_shares_them(machine_factory):
    m = await machine_factory.booted(devices={"I2C1": [Eeprom24(0x50, 4096)]}, name="m")
    assert m.name == "m"
    assert m.pc == m.sym("main")
    again = machine_factory(devices={"I2C1": [Eeprom24(0x50, 4096)]})
    assert again.pc == m.pc
"""


def test_device_sets_with_one_class_and_state_share_a_boot(
    pytester: pytest.Pytester, u575_project: Path
) -> None:
    pytester.makepyfile(KEYED)
    first = pytester.runpytest(*BASE)
    first.assert_outcomes(passed=2)
    assert summary(first) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"
    second = pytester.runpytest(*BASE)
    second.assert_outcomes(passed=2)
    assert summary(second) == "popcorn: 2 key(s), 0 boot(s), 2 from cache"


UNKEYED = """
import pytest
from api.devices_example import Eeprom24
from popcorn import ConfigError


class Sourced(Eeprom24):
    def __init__(self, source):
        super().__init__(0x50, 4096)
        self.source = source


def test_a_device_holding_a_function_boots_each_time(machine_factory):
    first = machine_factory(devices={"I2C1": [Sourced(lambda: 1)]})
    second = machine_factory(devices={"I2C1": [Sourced(lambda: 1)]})
    assert first.pc == second.pc == first.sym("main")


def test_a_device_serves_one_machine(machine_factory):
    machine_factory(devices={"I2C1": [Eeprom24(0x50, 4096)]})
    given = Eeprom24(0x50, 4096)
    machine_factory(booted=False, devices={"I2C1": [given]})
    with pytest.raises(ConfigError) as refused:
        machine_factory(devices={"I2C1": [given]})
    assert refused.value.key == "devices.I2C1"
    with pytest.raises(ConfigError):
        machine_factory(booted=False, devices={"I2C1": [given]})


def test_the_machine_fixture_keeps_the_project_devices(machine):
    (device,) = machine.board.devices("I2C1")
    assert isinstance(device, Eeprom24)
"""


def test_a_device_set_that_cannot_be_keyed_boots_each_time(
    pytester: pytest.Pytester, u575_project: Path
) -> None:
    pytester.makepyfile(UNKEYED)
    first = pytester.runpytest(*BASE)
    first.assert_outcomes(passed=3)
    assert summary(first) == "popcorn: 3 key(s), 4 boot(s), 0 from cache"
    second = pytester.runpytest(*BASE)
    second.assert_outcomes(passed=3)
    assert summary(second) == "popcorn: 3 key(s), 2 boot(s), 2 from cache"
    second.stdout.no_fnmatch_line("*dropped*")


REUSE = """
import pytest
from api.devices_example import Eeprom24
from popcorn import ConfigError


def test_reuse(machine_factory, request):
    given = Eeprom24(0x50, 4096)
    marker = request.config.rootpath / "booted"
    if not marker.exists():
        machine_factory(devices={"I2C1": [given]})
        marker.write_text("")
        return
    machine_factory(booted=False, devices={"I2C1": [given]})
    with pytest.raises(ConfigError):
        machine_factory(devices={"I2C1": [given]})
"""


def test_a_device_given_twice_keeps_the_cached_boot(
    pytester: pytest.Pytester, u575_project: Path
) -> None:
    pytester.makepyfile(REUSE)
    first = pytester.runpytest(*BASE)
    first.assert_outcomes(passed=1)
    assert summary(first) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    second = pytester.runpytest(*BASE)
    second.assert_outcomes(passed=1)
    second.stdout.no_fnmatch_line("popcorn: *")


BOOT_ONCE = """
from api.devices_example import Eeprom24


def test_boot(machine_factory):
    machine_factory(devices={"I2C1": [Eeprom24(0x50, 4096)]})
"""

BAD_NAME = """
import pytest
from api.devices_example import Eeprom24


def test_a_bad_name_leaves_the_cached_boot(machine_factory):
    with pytest.raises(ValueError, match="name"):
        machine_factory(devices={"I2C1": [Eeprom24(0x50, 4096)]}, name="a.b")
    with pytest.raises(ValueError, match="name"):
        machine_factory(name="a b")
    m = machine_factory(devices={"I2C1": [Eeprom24(0x50, 4096)]}, name="ok")
    assert m.name == "ok"
    assert m.pc == m.sym("main")
"""

BAD_NAME_ASYNC = """
import pytest
from api.devices_example import Eeprom24


async def test_a_bad_name_leaves_the_cached_boot(machine_factory):
    with pytest.raises(ValueError, match="name"):
        await machine_factory.booted(devices={"I2C1": [Eeprom24(0x50, 4096)]}, name="a.b")
    m = await machine_factory.booted(devices={"I2C1": [Eeprom24(0x50, 4096)]}, name="ok")
    assert m.name == "ok"
    assert m.pc == m.sym("main")
"""


def test_a_bad_name_is_refused_before_the_boot_cache_is_read(
    pytester: pytest.Pytester, u575_project: Path
) -> None:
    pytester.makepyfile(BOOT_ONCE)
    first = pytester.runpytest(*BASE)
    first.assert_outcomes(passed=1)
    assert summary(first) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    for later in (BAD_NAME, BAD_NAME_ASYNC):
        pytester.makepyfile(later)
        result = pytester.runpytest(*BASE)
        result.assert_outcomes(passed=1)
        assert summary(result) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
        result.stdout.no_fnmatch_line("*dropped*")


DIGEST = """
from pathlib import Path

import popcorn
import pytest
from popcorn import ResetKind
from popcorn.board import I2CTarget
from popcorn.devices import TableTarget
from popcorn.pytest._cache import devices_digest

M33_JSON = "{m33}"


class Plain(I2CTarget):
    level: int

    def __init__(self, extra: object = None) -> None:
        super().__init__(0x50)
        self.level = 0
        self.extra = extra
        self.answer = self.on_read


@pytest.fixture(scope="module")
def m33_project():
    return popcorn.Project(part="POPCORN-M33-TEST", description=Path(M33_JSON), images=[])


@pytest.mark.parametrize(
    "extra",
    [
        None,
        b"\\x01",
        bytearray(b"\\x02"),
        1.5,
        ResetKind.POWER_ON,
        [1, "a"],
        (2,),
        {3, 4},
        frozenset({5}),
        {"k": [True]},
    ],
)
def test_plain_attributes_key_a_device(m33_project: popcorn.Project, extra: object) -> None:
    digest = devices_digest(m33_project, {"TESTDEV": [Plain(extra)]})
    assert digest is not None
    assert digest == devices_digest(m33_project, {"TESTDEV": [Plain(extra)]})
    assert digest != devices_digest(m33_project, {"TESTDEV": [Plain("other")]})


@pytest.mark.parametrize(
    "extra",
    [print, object(), [object()], {"k": object()}, {object(): 1}, Plain().on_read],
)
def test_other_attributes_leave_a_device_unkeyed(
    m33_project: popcorn.Project, extra: object
) -> None:
    assert devices_digest(m33_project, {"TESTDEV": [Plain(extra)]}) is None


def test_the_digest_follows_state_aliases_and_listed_peripherals(
    m33_project: popcorn.Project,
) -> None:
    base = devices_digest(m33_project, {"TESTDEV": [Plain()]})
    moved = Plain()
    moved.level = 1
    assert devices_digest(m33_project, {"TESTDEV": [moved]}) != base
    alias = next(p.name for p in m33_project.machine_description().peripherals if p.alias_of)
    assert devices_digest(m33_project, {alias: [Plain()]}) == base
    assert devices_digest(m33_project, {"TESTDEV": []}) != devices_digest(m33_project, {})
    assert devices_digest(m33_project, {"TESTDEV": [Plain(), Plain()]}) != base
    assert devices_digest(m33_project, {"TESTDEV": [Plain({1, 9})]}) == devices_digest(
        m33_project, {"TESTDEV": [Plain({9, 1})]}
    )


class Shared(Plain):
    table = bytearray(1)

    def snapshot(self) -> bytes:
        return bytes(self.table)


def test_the_digest_follows_a_snapshot_outside_the_attributes(
    m33_project: popcorn.Project,
) -> None:
    before = devices_digest(m33_project, {"TESTDEV": [Shared()]})
    Shared.table[0] = 1
    assert devices_digest(m33_project, {"TESTDEV": [Shared()]}) != before


def test_a_native_device_leaves_the_set_unkeyed(m33_project: popcorn.Project) -> None:
    assert devices_digest(m33_project, {"TESTDEV": [TableTarget(0x50)]}) is None
"""


def test_the_device_digest(pytester: pytest.Pytester) -> None:
    pytester.makepyfile(DIGEST.replace("{m33}", str(M33_JSON)))
    pytester.runpytest(*BASE).assert_outcomes(passed=19)

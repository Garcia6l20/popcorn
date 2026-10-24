"""Several descriptions in one session: each test gets the `machine` fixture declared in the
conftest.py nearest above it."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import (
    DRIVER_ELF,
    FAULT_ELF,
    M33_JSON,
    write_conftest,
    write_project,
    write_project_conftest,
)

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

DRIVER_TEST = """
def test_driver(machine):
    assert machine.pc == machine.sym("alu_mark")
"""

FAULT_TEST = """
def test_fault(machine):
    assert machine.pc == machine.sym("ready")
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def summary(result: pytest.RunResult) -> str:
    return next(
        line for line in result.outlines if line.startswith("popcorn: ") and "key(s)" in line
    )


def two_projects(pytester: pytest.Pytester) -> tuple[Path, Path]:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    driver = pytester.mkdir("driver")
    fault = pytester.mkdir("fault")
    write_project(pytester, directory=driver, extra='name="driver",')
    write_project(pytester, until="ready", directory=fault, elf=FAULT_ELF, extra='name="fault",')
    (driver / "test_driver.py").write_text(DRIVER_TEST)
    (fault / "test_fault.py").write_text(FAULT_TEST)
    return driver, fault


def test_each_test_boots_the_project_nearest_above_it(pytester: pytest.Pytester) -> None:
    driver, fault = two_projects(pytester)
    for directory in (driver, fault):
        (directory / f"test_{directory.name}_failing.py").write_text(
            "def test_failing(machine):\n    assert False\n"
        )
    result = run(pytester)
    result.assert_outcomes(passed=2, failed=2)
    result.stdout.fnmatch_lines_random(
        [
            "machine (description=driver config=- digest=* strict=False):",
            "machine (description=fault config=- digest=* strict=False):",
        ]
    )
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


@pytest.mark.parametrize("workers", ["0", "2"])
def test_workers_boot_each_project(pytester: pytest.Pytester, workers: str) -> None:
    pytest.importorskip("xdist")
    two_projects(pytester)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", workers)
    result.assert_outcomes(passed=2)


def test_workers_share_one_cold_boot_per_project(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    two_projects(pytester)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", "2", "--dist", "each", timeout=12)
    result.assert_outcomes(passed=4)
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 2 from cache"


def test_a_factory_builds_another_project(pytester: pytest.Pytester) -> None:
    _, fault = two_projects(pytester)
    (fault / "test_other.py").write_text(f"""
from pathlib import Path

import popcorn
from popcorn.pytest._session import popcorn_session

DRIVER = popcorn.MachineDescription(
    part="POPCORN-M33-TEST",
    description=Path({str(M33_JSON)!r}),
    images=[popcorn.Elf(Path({str(DRIVER_ELF)!r}))],
    boot=popcorn.Boot(until="alu_mark", budget=1_000_000),
    strict=True,
)


def test_other(machine, machine_factory, request):
    assert machine.pc == machine.sym("ready")
    driver = machine_factory(DRIVER)
    assert driver.pc == driver.sym("alu_mark")
    assert machine_factory(DRIVER, booted=False).pc != driver.pc
    keys = popcorn_session(request.config).keys
    assert [key.strict for key in keys if key.digest == DRIVER.digest] == [True]
""")
    result = run(pytester)
    result.assert_outcomes(passed=3)
    assert summary(result) == "popcorn: 3 key(s), 3 boot(s), 0 from cache"


def test_a_nested_test_directory_uses_its_parent_project(pytester: pytest.Pytester) -> None:
    driver, _ = two_projects(pytester)
    nested = driver / "deeper" / "still"
    nested.mkdir(parents=True)
    (nested / "test_nested.py").write_text(DRIVER_TEST.replace("test_driver", "test_nested"))
    result = run(pytester)
    result.assert_outcomes(passed=3)
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


def test_the_rootdir_project_and_a_subproject(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    write_project(pytester)
    fault = pytester.mkdir("fault")
    write_project(pytester, until="ready", directory=fault, elf=FAULT_ELF, extra='name="fault",')
    (pytester.path / "test_root.py").write_text(DRIVER_TEST)
    (fault / "test_fault.py").write_text(FAULT_TEST + "    assert False\n")
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1)
    result.stdout.fnmatch_lines(["machine (description=fault config=- digest=* strict=False):"])
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


def test_a_project_above_the_rootdir_is_ignored(pytester: pytest.Pytester) -> None:
    write_project(pytester)
    example = pytester.mkdir("example")
    (example / "pytest.ini").write_text("[pytest]\n")
    (example / "conftest.py").write_text('pytest_plugins = ["popcorn.pytest"]\n')
    (example / "test_example.py").write_text(DRIVER_TEST)
    result = run(pytester, "example")
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*fixture 'machine' not found*"])


def test_the_project_fixture_is_the_test_project(pytester: pytest.Pytester) -> None:
    driver, fault = two_projects(pytester)
    for directory, name in ((driver, "driver"), (fault, "fault")):
        (directory / f"test_{name}_project.py").write_text(
            "def test_project(machine, description):\n"
            f"    assert description.name == {name!r}\n"
            "    assert machine.config_digest == description.digest\n"
        )
    run(pytester).assert_outcomes(passed=4)


MISSING_TESTS = """
import asyncio


def test_machine(machine):
    pass


def test_factory(machine_factory, description):
    machine_factory(description)


def test_factory_booted(machine_factory, description):
    asyncio.run(machine_factory.booted(description))


def test_factory_not_booted(machine_factory, description):
    machine_factory(description, booted=False)
"""


def missing_project(pytester: pytest.Pytester, extra: str = "") -> Path:
    absent = pytester.path / "absent.elf"
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    write_project(pytester, elf=absent, extra=extra)
    pytester.makepyfile(test_missing=MISSING_TESTS)
    return absent


@pytest.mark.parametrize("cache", [(), ("-p", "no:cacheprovider")], ids=["cache", "no-cache"])
def test_a_missing_input_skips_with_missing_skip(
    pytester: pytest.Pytester, cache: tuple[str, ...]
) -> None:
    absent = missing_project(pytester, 'missing="skip",')
    result = run(pytester, "-v", "-rs", *cache)
    result.assert_outcomes(skipped=4)
    assert result.ret == 0
    names = ("machine", "factory", "factory_booted", "factory_not_booted")
    result.stdout.fnmatch_lines_random([f"*::test_{name} SKIPPED *" for name in names])
    reasons = [line for line in result.outlines if line.startswith("SKIPPED [")]
    assert all(line.endswith(f": missing input: {absent}") for line in reasons)
    assert sum(int(line[len("SKIPPED [") :].split("]")[0]) for line in reasons) == 4


def test_a_missing_plugin_library_skips_with_missing_skip(pytester: pytest.Pytester) -> None:
    absent = pytester.path / "libkit.so"
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    write_project(
        pytester,
        extra=f'missing="skip", plugins=[Plugin("kit", library=Path({str(absent)!r}))],',
        imports="from popcorn.plugin import Plugin",
    )
    pytester.makepyfile(test_missing=MISSING_TESTS)
    result = run(pytester, "-rs")
    result.assert_outcomes(skipped=4)
    reasons = [line for line in result.outlines if line.startswith("SKIPPED [")]
    assert reasons and all(line.endswith(f": missing input: {absent}") for line in reasons)


def test_a_missing_input_skips_under_workers(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    missing_project(pytester, 'missing="skip",')
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", "2", timeout=12)
    result.assert_outcomes(skipped=4)


def test_a_missing_input_fails_by_default(pytester: pytest.Pytester) -> None:
    missing_project(pytester)
    result = run(pytester)
    result.assert_outcomes(failed=3, errors=1)
    result.stdout.fnmatch_lines(["*MissingInput: images[[]0[]].path: no file at *absent.elf*"])


DEVICES_TEST = """
from popcorn.board import I2CTarget


class Target(I2CTarget):
    pass


def test_factory_devices(machine_factory, description):
    machine_factory(description, devices={"I2C1": [Target(0x50)]})
"""


def test_a_missing_description_skips_a_factory_with_devices(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    pytester.makeini("[pytest]\n")
    absent = pytester.path / "absent.json"
    write_project_conftest(
        pytester.path,
        "popcorn.MachineDescription(\n"
        '    part="POPCORN-M33-TEST",\n'
        f"    description=Path({str(absent)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(DRIVER_ELF)!r}))],\n"
        '    missing="skip",\n'
        ")",
    )
    pytester.makepyfile(test_devices=DEVICES_TEST)
    result = run(pytester, "-rs")
    result.assert_outcomes(skipped=1)
    result.stdout.fnmatch_lines([f"SKIPPED * missing input: {absent}"])

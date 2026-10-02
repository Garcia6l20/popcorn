"""Several project files in one session: each test boots the `popcorn.toml` nearest above it."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import FAULT_ELF, write_conftest, write_project

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
    write_project(pytester, directory=driver)
    write_project(pytester, until="ready", directory=fault, elf=FAULT_ELF)
    (driver / "test_driver.py").write_text(DRIVER_TEST)
    (fault / "test_fault.py").write_text(FAULT_TEST)
    return driver, fault


def test_each_test_boots_the_project_nearest_above_it(pytester: pytest.Pytester) -> None:
    two_projects(pytester)
    result = run(pytester)
    result.assert_outcomes(passed=2)
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


@pytest.mark.parametrize("workers", ["0", "2"])
def test_workers_boot_each_project(pytester: pytest.Pytester, workers: str) -> None:
    pytest.importorskip("xdist")
    two_projects(pytester)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", workers)
    result.assert_outcomes(passed=2)


def test_the_project_option_serves_every_test(pytester: pytest.Pytester) -> None:
    driver, _ = two_projects(pytester)
    result = run(pytester, f"--popcorn-project={driver / 'popcorn.toml'}")
    result.assert_outcomes(passed=1, failed=1)
    result.stdout.fnmatch_lines(["FAILED fault/test_fault.py::test_fault*"])


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
    write_project(pytester, until="ready", directory=fault, elf=FAULT_ELF)
    (pytester.path / "test_root.py").write_text(DRIVER_TEST)
    (fault / "test_fault.py").write_text(FAULT_TEST + "    assert False\n")
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1)
    result.stdout.fnmatch_lines(["machine (project=fault/popcorn.toml config=- strict=False):"])
    assert summary(result) == "popcorn: 2 key(s), 2 boot(s), 0 from cache"


def test_a_project_file_above_the_rootdir_is_ignored(pytester: pytest.Pytester) -> None:
    write_project(pytester)
    example = pytester.mkdir("example")
    (example / "pytest.ini").write_text("[pytest]\n")
    (example / "conftest.py").write_text('pytest_plugins = ["popcorn.pytest"]\n')
    (example / "test_example.py").write_text(DRIVER_TEST)
    result = run(pytester, "example")
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines([f"*no project file {example / 'popcorn.toml'}*up to {example}*"])


def test_the_project_fixture_is_the_test_project(pytester: pytest.Pytester) -> None:
    driver, fault = two_projects(pytester)
    for directory, name in ((driver, "driver"), (fault, "fault")):
        (directory / f"test_{name}_project.py").write_text(
            "def test_project(popcorn_project):\n"
            f"    assert str(popcorn_project.path) == {str(directory / 'popcorn.toml')!r}\n"
        )
    run(pytester).assert_outcomes(passed=4)

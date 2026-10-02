"""The `pytest11` entry point: the plugin loads without a conftest, `-p no:` turns it off."""

from __future__ import annotations

import pytest

from .helpers import write_conftest, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

BOOTED = """
def test_booted(machine):
    assert machine.pc == machine.sym("alu_mark")
"""


def test_the_plugin_loads_without_a_conftest(pytester: pytest.Pytester) -> None:
    write_project(pytester)
    pytester.makepyfile(BOOTED)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-W", "error::pytest.PytestWarning")
    result.assert_outcomes(passed=1)


def test_a_conftest_naming_the_plugin_still_loads_it_once(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(BOOTED)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-W", "error::pytest.PytestWarning")
    result.assert_outcomes(passed=1)


def test_the_plugin_is_disabled_by_name(pytester: pytest.Pytester) -> None:
    write_project(pytester)
    pytester.makepyfile(BOOTED)
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-p", "no:popcorn.pytest")
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines(["*fixture 'machine' not found*"])

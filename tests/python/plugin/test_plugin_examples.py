"""The plugin example of spec 03 section 10 runs as a suite would run it."""

from __future__ import annotations

from pathlib import Path

import pytest

from .helpers import write_project

pytest_plugins = ["pytester"]

EXAMPLES = Path(__file__).parent / "examples"


def test_the_example_suite_runs(pytester: pytest.Pytester) -> None:
    (pytester.path / "conftest.py").write_text((EXAMPLES / "example_conftest.py").read_text())
    (pytester.path / "test_routing.py").write_text((EXAMPLES / "example_routing.py").read_text())
    write_project(pytester)
    result = pytester.runpytest("-p", "no:asyncio")
    result.assert_outcomes(passed=3)
    result.stdout.fnmatch_lines(["popcorn: 1 key(s), 1 boot(s), 0 from cache"])


def test_help_lists_the_options_and_marker(pytester: pytest.Pytester) -> None:
    (pytester.path / "conftest.py").write_text((EXAMPLES / "example_conftest.py").read_text())
    options = pytester.runpytest("-p", "no:asyncio", "--help")
    options.stdout.fnmatch_lines(
        [
            "popcorn firmware emulation:",
            "*--popcorn-cov*",
            "*--popcorn-trace-on-fail*",
            "*--popcorn-strict*",
            "*--popcorn-budget*",
            "*--popcorn-part*",
            "*--popcorn-ring*",
        ]
    )
    markers = pytester.runpytest("-p", "no:asyncio", "--markers")
    markers.stdout.fnmatch_lines(["@pytest.mark.popcorn(budget=None, strict=None):*"])

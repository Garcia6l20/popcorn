"""Dev-mode native plugins built when the directory whose `conftest.py` declared them is collected.

Under pytest-xdist the controller collects nothing, so every worker runs this hook. The cache's
file lock makes the build run once. A failure becomes the directory's collection error, and no
test of the directory runs.
"""

from __future__ import annotations

from collections.abc import Generator
from pathlib import Path

import pytest

from popcorn._errors import ConfigError
from popcorn.plugin import Plugin
from popcorn.plugin._records import constructed


def declared_by(directory: Path) -> list[Plugin]:
    """The dev plugins the `conftest.py` of `directory` constructed, in order."""
    conftest = (directory / "conftest.py").resolve()
    return [plugin for plugin, origin in constructed() if origin == conftest]


@pytest.hookimpl(wrapper=True)
def pytest_make_collect_report(
    collector: pytest.Collector,
) -> Generator[None, pytest.CollectReport, pytest.CollectReport]:
    report = yield
    if not isinstance(collector, pytest.Directory) or not report.passed:
        return report
    for plugin in declared_by(collector.path):
        try:
            plugin.built()
        except ConfigError as error:
            return pytest.CollectReport(collector.nodeid, "failed", str(error), [])
    return report

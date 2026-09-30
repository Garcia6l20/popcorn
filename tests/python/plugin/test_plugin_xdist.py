"""pytest-xdist: a machine per worker, the same outcomes and lcov as without, FR-PYT-3."""

from __future__ import annotations

import xml.etree.ElementTree as ElementTree
from pathlib import Path

import pytest

from .coverage_suite import NODE_IDS, write_suite

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")


@pytest.fixture(autouse=True)
def _needs_xdist() -> None:
    pytest.importorskip("xdist")


def outcomes(junit: Path) -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for case in ElementTree.parse(junit).getroot().iter("testcase"):
        kinds = [child.tag for child in case if child.tag in ("failure", "error", "skipped")]
        found.append((f"{case.get('classname')}::{case.get('name')}", kinds[0] if kinds else "ok"))
    return sorted(found)


def covered(
    pytester: pytest.Pytester, name: str, *args: str
) -> tuple[bytes, list[tuple[str, str]]]:
    result = pytester.runpytest_subprocess(
        *BASE, f"--popcorn-cov={name}.info", f"--junitxml={name}.xml", *args
    )
    result.assert_outcomes(passed=30)
    return (pytester.path / f"{name}.info").read_bytes(), outcomes(pytester.path / f"{name}.xml")


def test_workers_give_the_same_outcomes_and_lcov(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    serial, serial_outcomes = covered(pytester, "n0", "-n", "0")
    reversed_order, reversed_outcomes = covered(pytester, "rev", "-n", "0", *reversed(NODE_IDS))
    two, two_outcomes = covered(pytester, "n2", "-n", "2")
    four, four_outcomes = covered(pytester, "n4", "-n", "4")
    assert serial_outcomes == reversed_outcomes == two_outcomes == four_outcomes
    assert len(serial_outcomes) == 30
    assert reversed_order == serial
    assert two == serial
    assert four == serial


def test_each_worker_boots_and_saves_its_own_file(pytester: pytest.Pytester) -> None:
    write_suite(
        pytester,
        """
        from pathlib import Path

        import pytest

        @pytest.hookimpl(trylast=True)
        def pytest_sessionfinish(session):
            from popcorn.pytest._session import COVERAGE

            if getattr(session.config, "workerinput", None) is not None:
                return
            run = session.config.stash[COVERAGE]
            names = sorted(path.name for path in run.directory.iterdir())
            Path("files.txt").write_text("\\n".join(names) + "\\n" + str(run.directory))
        """,
    )
    result = pytester.runpytest_subprocess(
        *BASE, "-n", "2", "--dist", "each", "--popcorn-cov=a.info"
    )
    result.assert_outcomes(passed=60)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 4 boot(s), 0 from cache"])
    *names, directory = (pytester.path / "files.txt").read_text().splitlines()
    assert names[-2:] == ["gw0.pcov", "gw1.pcov"]
    assert len(names) == 4 and all(name.startswith("boot-") for name in names[:2])
    assert not Path(directory).exists()


def test_without_workers_the_process_boots_once_per_key(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest_subprocess(*BASE, "-n", "0")
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines(["popcorn: 2 key(s), 2 boot(s), 0 from cache"])

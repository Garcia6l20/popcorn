"""UC-2: setup plus teardown of a declared machine fixture under 1 ms median per test.

Opt-in, `POPCORN_BENCH=1`, on a quiet host.
"""

from __future__ import annotations

import json
import os
import statistics

import pytest

from .helpers import write_conftest, write_project

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

pytest_plugins = ["pytester"]

TESTS = 500

TIMING = """
import json
import time
from pathlib import Path

import pytest

TIMES = []


@pytest.hookimpl(wrapper=True)
def pytest_runtest_setup(item):
    started = time.perf_counter()
    try:
        return (yield)
    finally:
        item.stash_started = time.perf_counter() - started


@pytest.hookimpl(wrapper=True)
def pytest_runtest_teardown(item):
    started = time.perf_counter()
    try:
        return (yield)
    finally:
        TIMES.append(item.stash_started + time.perf_counter() - started)


def pytest_unconfigure(config):
    Path("times.json").write_text(json.dumps(TIMES[1:]))
"""


def test_machine_fixture_overhead(pytester: pytest.Pytester) -> None:
    write_conftest(pytester, TIMING)
    write_project(pytester)
    pytester.makepyfile(f"""
        import pytest

        @pytest.mark.parametrize("n", range({TESTS}))
        def test_nothing(machine, n):
            pass
        """)
    pytester.runpytest("-p", "no:asyncio").assert_outcomes(passed=TESTS)
    times: list[float] = sorted(json.loads((pytester.path / "times.json").read_text()))
    median = statistics.median(times)
    p95 = times[int(len(times) * 0.95)]
    print(f"\nUC-2: {len(times)} tests, median {median * 1e6:.0f} us, p95 {p95 * 1e6:.0f} us")
    assert median < 1e-3

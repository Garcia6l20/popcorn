"""Costs the plugin plan left to measure on a quiet host: `--popcorn-trace-on-fail` over 20
tests, and hashing the native extension for the boot cache key.

Opt-in, `POPCORN_BENCH=1`, on a quiet host. Informational: nothing is asserted.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
from popcorn import _core

from .helpers import write_conftest, write_project

pytestmark = pytest.mark.skipif(
    os.environ.get("POPCORN_BENCH") != "1", reason="set POPCORN_BENCH=1 on a quiet host"
)

pytest_plugins = ["pytester"]

SUITE = """
import pytest

@pytest.mark.parametrize("n", range(20))
async def test_call(machine, n):
    await machine.call("alu_mark")
    assert machine.mem.u32[machine.sym("alu_rounds")] == 1
"""


def test_trace_on_fail_cost(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(SUITE)
    base = [sys.executable, "-m", "pytest", "-q", "-p", "no:asyncio", "-p", "no:cacheprovider"]
    walls: dict[str, float] = {}
    for name, extra in (("plain", []), ("traced", ["--popcorn-trace-on-fail=traces"])):
        started = time.perf_counter()
        subprocess.run([*base, *extra], cwd=pytester.path, check=True, capture_output=True)
        walls[name] = time.perf_counter() - started
    print(
        f"\ntrace on fail: 20 tests, plain {walls['plain']:.3f} s, "
        f"traced {walls['traced']:.3f} s, ratio {walls['traced'] / walls['plain']:.2f}"
    )


def test_extension_hash_cost() -> None:
    path = Path(str(_core.__file__))
    started = time.perf_counter()
    hashlib.sha256(path.read_bytes()).hexdigest()
    elapsed = time.perf_counter() - started
    print(f"\n_core hash: {path.stat().st_size} bytes in {elapsed * 1e3:.1f} ms, bar 50 ms")

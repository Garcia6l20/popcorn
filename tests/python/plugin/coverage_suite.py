"""The step 06 suite: two configurations, 30 tests, for coverage and xdist runs."""

from __future__ import annotations

import pytest

from .helpers import CALLEE_ELF, DRIVER_ELF, M33_JSON, write_conftest

PROJECT = f"""
[mcu]
part = "POPCORN-M33-TEST"
description = "{M33_JSON}"

[image.app]
elf = "{CALLEE_ELF}"

[configs.booted]
image.app.elf = "{DRIVER_ELF}"
boot.until = "alu_mark"
"""

TESTS = """
import pytest

@pytest.mark.parametrize("a, b", [(1, 2), (-3, 4), (100, 200), (0, 0), (7, -7), (5, 5)])
async def test_add(machine, a, b):
    assert (await machine.call("add", a, b)).value == a + b

@pytest.mark.parametrize("a", [1, -2, 300, 0, 12345, -1])
async def test_negate(machine, a):
    assert (await machine.call("negate", a)).value == -a

@pytest.mark.parametrize("n", range(4))
async def test_ones(machine, n):
    assert (await machine.call("ones")).value == 0xFFFFFFFF

@pytest.mark.parametrize("n", range(4))
async def test_bump(machine, n):
    assert (await machine.call("bump")).value == 1

@pytest.mark.parametrize("v", [0, 1, 128, 255])
async def test_low_byte(machine, v):
    assert (await machine.call("low_byte", v)).value == v

@pytest.mark.popcorn(config="booted")
@pytest.mark.parametrize("n", range(6))
async def test_alu_mark(machine, n):
    await machine.call("alu_mark")
    assert machine.mem.u32[machine.sym("alu_rounds")] == 1
"""

NODE_IDS = (
    [f"test_suite.py::test_add[{a}-{b}]" for a, b in ((1, 2), (-3, 4), (100, 200), (0, 0))]
    + [f"test_suite.py::test_add[{a}-{b}]" for a, b in ((7, -7), (5, 5))]
    + [f"test_suite.py::test_negate[{a}]" for a in (1, -2, 300, 0, 12345, -1)]
    + [f"test_suite.py::test_ones[{n}]" for n in range(4)]
    + [f"test_suite.py::test_bump[{n}]" for n in range(4)]
    + [f"test_suite.py::test_low_byte[{v}]" for v in (0, 1, 128, 255)]
    + [f"test_suite.py::test_alu_mark[{n}]" for n in range(6)]
)


def write_suite(pytester: pytest.Pytester, conftest: str = "") -> None:
    """The project, the conftest with `conftest` appended and `test_suite.py`."""
    write_conftest(pytester, conftest)
    (pytester.path / "popcorn.toml").write_text(PROJECT)
    (pytester.path / "test_suite.py").write_text(TESTS)

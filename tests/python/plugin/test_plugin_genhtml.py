"""`--popcorn-cov` output genhtml 2 reads with branch coverage and no ignored error."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from .helpers import M33_JSON, ROOT, write_conftest

pytest_plugins = ["pytester"]

GENHTML = shutil.which("genhtml")
CUT_ELF = ROOT / "tests" / "firmware" / "obs" / "prebuilt" / "cut.elf"

PROJECT = f"""
[mcu]
part = "POPCORN-M33-TEST"
description = "{M33_JSON}"

[image.app]
elf = "{CUT_ELF}"

[boot]
until = "cut_mid"
"""

TESTS = """
import popcorn

async def test_intercepted_at_entry(machine):
    machine.intercept("pick", lambda call: call.returns(7))
    assert (await machine.call("use_pick", 5)).value == 14

async def test_faulting_load(machine):
    result = await machine.call("probe", 0x6000_0000)
    assert result.stop.reason == popcorn.StopReason.FAULT
"""


def session(pytester: pytest.Pytester, name: str, *args: str) -> Path:
    """The lcov file of one session over `cut.elf`, booted to a stop inside a block."""
    write_conftest(pytester)
    (pytester.path / "popcorn.toml").write_text(PROJECT)
    (pytester.path / "test_cut.py").write_text(TESTS)
    lcov = pytester.path / f"{name}.info"
    result = pytester.runpytest("-p", "no:asyncio", *args, f"--popcorn-cov={lcov}")
    result.assert_outcomes(passed=2)
    return lcov


def genhtml(lcov: Path, out: Path) -> subprocess.CompletedProcess[str]:
    """genhtml with branch coverage over `lcov`, run from the repository its paths start at."""
    assert GENHTML is not None
    return subprocess.run(
        [GENHTML, "--branch-coverage", "-o", str(out), str(lcov)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.skipif(GENHTML is None, reason="genhtml is not installed")
@pytest.mark.xfail(strict=True, reason="cut-short blocks count as run, FN lines have no DA")
def test_blocks_cut_short_render_without_ignored_errors(pytester: pytest.Pytester) -> None:
    lcov = session(pytester, "plain", "-p", "no:xdist")
    rendered = genhtml(lcov, pytester.path / "html")
    assert rendered.returncode == 0, rendered.stderr
    assert "ERROR" not in rendered.stdout + rendered.stderr
    assert "WARNING" not in rendered.stdout + rendered.stderr
    assert (pytester.path / "html" / "index.html").is_file()

"""`--popcorn-cov` output genhtml 2 reads with branch coverage and no ignored error."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from .helpers import M33_JSON, ROOT, write_conftest

pytest_plugins = ["pytester"]

GENHTML = shutil.which("genhtml")
OBS_FIRMWARE = ROOT / "tests" / "firmware" / "obs"
CUT_ELF = OBS_FIRMWARE / "prebuilt" / "cut.elf"
SHARED_EXIT_ELF = OBS_FIRMWARE / "prebuilt" / "shared_exit.elf"
SHARED_EXIT_S = OBS_FIRMWARE / "shared_exit.S"

CUT_TESTS = """
import popcorn

async def test_intercepted_at_entry(machine):
    machine.intercept("pick", lambda call: call.returns(7))
    assert (await machine.call("use_pick", 5)).value == 14

async def test_faulting_load(machine):
    result = await machine.call("probe", 0x6000_0000)
    assert result.stop.reason == popcorn.StopReason.FAULT
"""

SHARED_EXIT_TESTS = """
async def test_booted(machine):
    assert machine.pc == machine.sym("settled")
"""


def session(
    pytester: pytest.Pytester, elf: Path, until: str, tests: str, name: str, *args: str
) -> Path:
    """The lcov file of one session over `elf` booted until `until`, running `tests`."""
    write_conftest(pytester)
    (pytester.path / "popcorn.toml").write_text(
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{elf}"\n[boot]\nuntil = "{until}"\n'
    )
    (pytester.path / "test_cov.py").write_text(tests)
    lcov = pytester.path / f"{name}.info"
    result = pytester.runpytest("-p", "no:asyncio", *args, f"--popcorn-cov={lcov}")
    result.assert_outcomes(passed=tests.count("async def test_"))
    return lcov


def genhtml(lcov: Path, out: Path, *args: str) -> subprocess.CompletedProcess[str]:
    """genhtml with branch coverage over `lcov`, run from the repository its paths start at."""
    assert GENHTML is not None
    return subprocess.run(
        [GENHTML, "--branch-coverage", *args, "-o", str(out), str(lcov)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def messages(done: subprocess.CompletedProcess[str]) -> list[str]:
    """The ERROR and WARNING lines genhtml printed."""
    return [
        line
        for line in (done.stdout + done.stderr).splitlines()
        if "ERROR" in line or "WARNING" in line
    ]


@pytest.mark.skipif(GENHTML is None, reason="genhtml is not installed")
def test_blocks_cut_short_render_without_ignored_errors(pytester: pytest.Pytester) -> None:
    lcov = session(pytester, CUT_ELF, "cut_mid", CUT_TESTS, "plain", "-p", "no:xdist")
    rendered = genhtml(lcov, pytester.path / "html")
    assert rendered.returncode == 0, rendered.stderr
    assert messages(rendered) == []
    assert (pytester.path / "html" / "index.html").is_file()


def test_blocks_cut_short_export_the_same_bytes_across_workers(
    pytester: pytest.Pytester,
) -> None:
    plain = session(pytester, CUT_ELF, "cut_mid", CUT_TESTS, "plain", "-p", "no:xdist")
    split = session(pytester, CUT_ELF, "cut_mid", CUT_TESTS, "split", "-n", "2")
    assert plain.read_bytes() == split.read_bytes()


@pytest.mark.skipif(GENHTML is None, reason="genhtml is not installed")
def test_a_shared_exit_line_is_the_one_message_genhtml_reports(pytester: pytest.Pytester) -> None:
    lcov = session(
        pytester, SHARED_EXIT_ELF, "settled", SHARED_EXIT_TESTS, "shared", "-p", "no:xdist"
    )
    line = next(
        number
        for number, text in enumerate(SHARED_EXIT_S.read_text().splitlines(), 1)
        if "cmp r0, #2" in text
    )
    expected = (
        f'"tests/firmware/obs/shared_exit.S":{line}: '
        "line is hit but no branches on line have been evaluated."
    )
    strict = genhtml(lcov, pytester.path / "strict")
    assert strict.returncode != 0
    assert expected in strict.stderr
    accepted = genhtml(lcov, pytester.path / "accepted", "--ignore-errors", "inconsistent")
    assert accepted.returncode == 0, accepted.stderr
    assert messages(accepted)
    assert all(expected in message for message in messages(accepted))

"""The example stages of the gate: collection per directory, one pytest per extra part."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from popcorn_build.check import stages
from popcorn_build.example_parts import EXAMPLE_PARTS, extra_runs
from popcorn_build.example_runs import (
    collect_commands,
    example_dirs,
    run_all,
)

ROOT = Path(__file__).resolve().parents[3]


def test_every_example_directory_has_a_row_and_a_collect_command() -> None:
    dirs = example_dirs()
    assert {path.removeprefix("examples/") for path in dirs} == set(EXAMPLE_PARTS)
    assert [name for name, _ in collect_commands()] == dirs
    assert all("--collect-only" in command for _, command in collect_commands())


@pytest.mark.parametrize(("jobs", "expected"), [(None, 8), (4, 4), (1, 1)])
def test_every_parallel_stage_takes_the_gate_jobs(jobs: int | None, expected: int) -> None:
    gate = dict(stages(False, jobs))
    workers = "0" if expected == 1 else str(expected)
    for name in ("pytest", "examples"):
        command = gate[name]
        assert command[command.index("-n") + 1] == workers
    for name in ("build", "test"):
        command = gate[name]
        assert command[command.index("-j") + 1] == str(expected)
    command = gate["examples-collect"]
    assert command[command.index("--jobs") + 1] == str(expected)


def test_the_gate_runs_the_examples_once_then_collects_each_directory() -> None:
    names = [name for name, _ in stages(False)]
    assert names.index("examples") + 1 == names.index("examples-collect")
    assert not any("/" in name and "@" not in name for name in names)


def test_each_extra_part_run_is_one_gate_stage() -> None:
    parted = sorted(name for name, _ in stages(False) if "@" in name)
    assert parted == sorted(
        f"{path.removeprefix('examples/')}@{part}" for path, part in extra_runs()
    )


def test_a_run_that_collects_no_test_fails_the_stage(tmp_path: Path) -> None:
    (tmp_path / "pytest.ini").write_text("[pytest]\n")
    (tmp_path / "test_nothing.py").write_text("x = 1\n")
    command = [sys.executable, "-m", "pytest", "-q", "--collect-only", "-p", "no:cacheprovider"]
    assert run_all([("empty", [*command, str(tmp_path)])], 1) == 1


def test_a_failing_run_fails_the_stage_and_prints_its_name(
    capsys: pytest.CaptureFixture[str],
) -> None:
    commands = [
        ("good", ["python", "-c", "pass"]),
        ("bad", ["python", "-c", "print('boom'); exit(3)"]),
    ]
    assert run_all(commands, 2) == 1
    out = capsys.readouterr().out
    assert "FAILED (3): bad" in out
    assert "boom" in out
    assert "1 of 2 runs passed" in out

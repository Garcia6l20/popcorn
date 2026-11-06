"""The example stages of the gate: collection per directory, one pytest per extra part."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import pytest

from popcorn_build import example_runs
from popcorn_build.check import stages
from popcorn_build.example_parts import EXAMPLE_PARTS, extra_runs
from popcorn_build.example_runs import (
    WIDE_EXAMPLES,
    Run,
    collect_commands,
    example_dirs,
    part_commands,
    part_runs,
    run_all,
    unproven,
)
from popcorn_build.part_ids import file_counts

ROOT = Path(__file__).resolve().parents[3]


def test_every_example_directory_has_a_row_and_a_collect_command() -> None:
    dirs = example_dirs()
    assert {path.removeprefix("examples/") for path in dirs} == set(EXAMPLE_PARTS)
    assert [run.name for run in collect_commands()] == dirs
    assert all("--collect-only" in run.command for run in collect_commands())


def test_the_part_runs_are_the_extra_runs_regrouped() -> None:
    regrouped = [(path, part) for part, paths in part_runs().items() for path in paths]
    assert sorted(regrouped) == sorted(extra_runs())
    assert len(regrouped) == len(set(regrouped))


def test_each_part_run_names_its_part_and_needs_a_test_on_it_in_every_directory() -> None:
    runs = {run.name: run for run in part_commands(8)}
    assert set(runs) == set(part_runs())
    for part, paths in part_runs().items():
        command = runs[part].command
        assert command.count(f"--popcorn-part={part}") == 1
        assert command[command.index(f"--popcorn-part={part}") + 1 :] == paths
        assert list(runs[part].proof) == paths


def test_the_wide_examples_run_first_on_their_workers() -> None:
    runs = part_commands(8)
    wide_parts = {
        part for part, paths in part_runs().items() if any(path in WIDE_EXAMPLES for path in paths)
    }
    assert wide_parts
    assert {run.name for run in runs[: len(wide_parts)]} == wide_parts
    for run in runs:
        workers = run.command[run.command.index("-n") + 1]
        assert (workers != "0") == (run.name in wide_parts)
        assert run.slots == max(int(workers), 1)


@pytest.mark.parametrize("jobs", [1, 2, 3, 8])
def test_no_part_run_needs_more_slots_than_the_jobs(jobs: int) -> None:
    assert all(run.slots <= jobs for run in part_commands(jobs))


def test_one_job_runs_every_part_serially() -> None:
    for run in part_commands(1):
        assert run.command[run.command.index("-n") + 1] == "0"


def test_every_part_run_has_the_per_test_timeout_of_the_project() -> None:
    for run in part_commands(8):
        assert "timeout=15" in run.command
        assert "timeout_method=thread" in run.command


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
    for name in ("examples-collect", "examples-parts"):
        command = gate[name]
        assert command[command.index("--jobs") + 1] == str(expected)


def test_the_gate_runs_the_examples_once_then_collects_and_runs_the_parts() -> None:
    names = [name for name, _ in stages(False)]
    assert names.index("examples") + 1 == names.index("examples-collect")
    assert names.index("examples-collect") + 1 == names.index("examples-parts")
    assert not any("@" in name or "/" in name for name in names)


def test_fail_fast_reaches_both_example_run_stages() -> None:
    gate = dict(stages(False, fail_fast=True))
    ordinary = dict(stages(False))
    for name in ("examples-collect", "examples-parts"):
        assert "--fail-fast" in gate[name]
        assert "--fail-fast" not in ordinary[name]


def python(code: str) -> list[str]:
    return [sys.executable, "-c", code]


def test_a_run_that_collects_no_test_fails_the_stage(tmp_path: Path) -> None:
    (tmp_path / "pytest.ini").write_text("[pytest]\n")
    (tmp_path / "test_nothing.py").write_text("x = 1\n")
    command = [sys.executable, "-m", "pytest", "-q", "--collect-only", "-p", "no:cacheprovider"]
    assert run_all([Run("empty", [*command, str(tmp_path)])], 1) == 1


def test_a_failing_run_fails_the_stage_and_prints_its_name(
    capsys: pytest.CaptureFixture[str],
) -> None:
    runs = [Run("good", python("pass")), Run("bad", python("print('boom'); exit(3)"))]
    assert run_all(runs, 2) == 1
    out = capsys.readouterr().out
    assert "FAILED (3): bad" in out
    assert "boom" in out
    assert "1 of 2 runs passed" in out


RAN_ON_PART = """
import json, sys
json.dump(%s, open(sys.argv[sys.argv.index('--part-ids-file') + 1], 'w'))
"""


def proof_run(name: str, ran: list[str], dirs: tuple[str, ...]) -> Run:
    files = {str(ROOT / path / "test_x.py"): 1 for path in ran}
    return Run(name, python(RAN_ON_PART % repr(files)), proof=dirs)


def test_a_directory_with_no_test_on_the_part_fails_the_run_that_others_satisfy(
    capsys: pytest.CaptureFixture[str],
) -> None:
    dirs = ("examples/stm32l476/rcc", "examples/stm32l476/gpio")
    assert run_all([proof_run("both", list(dirs), dirs)], 1) == 0
    assert run_all([proof_run("one", [dirs[0]], dirs)], 1) == 1
    out = capsys.readouterr().out
    assert "NO TEST RAN ON THE PART in examples/stm32l476/gpio: one" in out


def test_a_run_that_writes_no_proof_fails() -> None:
    run = Run("none", python("pass"), proof=("examples/stm32l476/rcc",))
    assert run_all([run], 1) == 1


def test_the_part_runs_take_the_stop_flag() -> None:
    assert all("-x" in run.command for run in part_commands(8, ["-x"]))
    assert not any("-x" in run.command for run in part_commands(8))


def test_file_counts_key_each_test_by_its_file_under_the_root(tmp_path: Path) -> None:
    nodes = [
        "a/test_one.py::test_x",
        "a/test_one.py::test_y[p]",
        "a/test_two.py::t",
        "b/c/test_z.py::t",
    ]
    counts = file_counts(nodes, tmp_path)
    assert counts == {
        str(tmp_path / "a/test_one.py"): 2,
        str(tmp_path / "a/test_two.py"): 1,
        str(tmp_path / "b/c/test_z.py"): 1,
    }


def test_a_directory_is_proven_by_any_of_its_files_and_only_its_own(tmp_path: Path) -> None:
    dirs = ("examples/stm32l476/rcc", "examples/stm32l476/rcc2", "examples/stm32l476/gpio")
    nodes = [
        "examples/stm32l476/rcc/sub/test_a.py::t",
        "examples/stm32l476/rcc/test_b.py::t",
        "examples/stm32l476/rcc2/test_c.py::t",
    ]
    ids = tmp_path / "ids.json"
    ids.write_text(json.dumps(file_counts(nodes, ROOT)))
    assert unproven(Run("p", [], proof=dirs), ids) == ["examples/stm32l476/gpio"]
    ids.write_text(json.dumps(file_counts(nodes[:1], ROOT)))
    assert unproven(Run("p", [], proof=dirs), ids) == list(dirs[1:])


def test_a_test_file_relative_to_an_example_rootdir_maps_to_that_directory() -> None:
    rootdir = ROOT / "examples/stm32l476/rcc"
    assert file_counts(["test_rcc.py::t"], rootdir) == {str(rootdir / "test_rcc.py"): 1}


def test_the_stage_never_runs_more_processes_than_the_jobs(tmp_path: Path) -> None:
    code = (
        "import sys, time, pathlib\n"
        "d = pathlib.Path(sys.argv[1]); n = int(sys.argv[2])\n"
        "(d / f'{time.monotonic_ns()}-start-{n}').touch()\n"
        "time.sleep(0.3)\n"
        "(d / f'{time.monotonic_ns()}-end-{n}').touch()\n"
    )
    runs = [
        Run(f"r{index}", [sys.executable, "-c", code, str(tmp_path), str(slots)], slots)
        for index, slots in enumerate([3, 3, 1, 2, 1, 3, 2, 2])
    ]
    assert run_all(runs, 4) == 0
    events = sorted(
        (int(path.name.split("-")[0]), path.name.split("-")[1], int(path.name.split("-")[2]))
        for path in tmp_path.iterdir()
    )
    held = peak = 0
    for _, kind, slots in events:
        held += slots if kind == "start" else -slots
        peak = max(peak, held)
    assert 0 < peak <= 4


def touch(path: Path) -> str:
    return f"import pathlib; pathlib.Path({str(path)!r}).touch()"


def test_fail_fast_returns_before_the_queue_drains(tmp_path: Path) -> None:
    runs = [
        Run("slow", python("import time; time.sleep(30)")),
        Run("bad", python("import time; time.sleep(0.5); exit(1)")),
        Run("late", python(touch(tmp_path / "late"))),
    ]
    start = time.monotonic()
    assert run_all(runs, 2, fail_fast=True) == 1
    assert time.monotonic() - start < 10
    assert not (tmp_path / "late").exists()


def test_a_run_over_the_timeout_is_killed_and_reported(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(example_runs, "RUN_TIMEOUT", 0.5)
    start = time.monotonic()
    assert (
        run_all(
            [Run("hung", python("import time; print('partial', flush=True); time.sleep(30)"))], 1
        )
        == 1
    )
    assert time.monotonic() - start < 10
    out = capsys.readouterr().out
    assert "TIMEOUT after 0.5 s: hung" in out
    assert "partial" in out


def alive(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except OSError:
        return False
    return state != "Z"


@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
def test_a_signal_kills_the_running_group_and_starts_no_queued_run(
    tmp_path: Path, signum: signal.Signals
) -> None:
    pidfile = tmp_path / "pid"
    sleeper = (
        "import os, time, pathlib, subprocess, sys\n"
        "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
        f"pathlib.Path({str(pidfile)!r}).write_text(str(child.pid))\n"
        "time.sleep(60)\n"
    )
    late = tmp_path / "late"
    driver = (
        "import sys\n"
        "from popcorn_build.example_runs import Run, run_all\n"
        f"runs = [Run('slow', [sys.executable, '-c', {sleeper!r}]),"
        f" Run('late', [sys.executable, '-c', {touch(late)!r}])]\n"
        "sys.exit(run_all(runs, 1))\n"
    )
    process = subprocess.Popen([sys.executable, "-c", driver], cwd=ROOT)
    try:
        deadline = time.monotonic() + 15
        while not pidfile.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        grandchild = int(pidfile.read_text())
        process.send_signal(signum)
        assert process.wait(timeout=15) == 128 + signum
    finally:
        process.kill()
    time.sleep(0.3)
    assert not late.exists()
    assert not alive(grandchild)


def test_a_signal_during_a_spawn_leaves_no_unlisted_group(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    before = signal.getsignal(signal.SIGTERM)
    spawned: list[subprocess.Popen[Any]] = []
    real = subprocess.Popen

    def spawn_then_signal(*args: Any, **kwargs: Any) -> subprocess.Popen[Any]:
        process = real(*args, **kwargs)
        spawned.append(process)
        os.kill(os.getpid(), signal.SIGTERM)
        return process

    monkeypatch.setattr(example_runs.subprocess, "Popen", spawn_then_signal)
    runs = [
        Run("first", python("import time; time.sleep(60)")),
        Run("late", python(touch(tmp_path / "late"))),
    ]
    assert run_all(runs, 2) == 128 + signal.SIGTERM
    assert len(spawned) == 1
    assert spawned[0].poll() is not None
    assert not (tmp_path / "late").exists()
    assert signal.getsignal(signal.SIGTERM) == before


@pytest.mark.timeout(5)
def test_a_run_wider_than_the_jobs_still_starts_alone(tmp_path: Path) -> None:
    run = Run("wide", python(touch(tmp_path / "ran")), slots=4)
    assert run_all([run], 2) == 0
    assert (tmp_path / "ran").exists()

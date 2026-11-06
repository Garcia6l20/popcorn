"""The example runs of the gate, each in its own pytest process, several at a time.

`python -m popcorn_build.example_runs collect --jobs N` runs `pytest --collect-only` once per
`examples/<chip>/<name>` directory with a `pytest.ini`, which proves each directory runs
standalone from its own rootdir. A directory that collects no test fails.
`python -m popcorn_build.example_runs parts --jobs N` runs, per extra part of `EXAMPLE_PARTS`, one
pytest over every example directory listing that part through `--popcorn-part`. A run fails when
no test of one of its directories built a machine on the part and did not skip.
At most N pytest processes run at a time, xdist workers included. A run that exceeds `RUN_TIMEOUT`
is killed. The output of each failing run is printed when it ends.
"""

from __future__ import annotations

import contextlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from types import FrameType
from typing import IO, Final

import click

from popcorn_build.config import project_dir
from popcorn_build.example_parts import extra_runs

WIDE_EXAMPLES: Final = {"examples/stm32u575/linked_uart": 4}
"""Examples whose tests are few and long, with the xdist workers their part run takes. The
others run on one worker: a pytest process per part already keeps the host busy, and extra
workers per process cost more start-up than they save."""

RUN_TIMEOUT: Final = 60.0
"""Seconds one run may take. The slowest part run takes 10 s on four workers and 22 s on one, and
a single test is bounded by `TEST_TIMEOUT_ARGS` at 15 s."""

TEST_TIMEOUT_ARGS: Final = ("-o", "timeout=15", "-o", "timeout_method=thread")
"""The per-test timeout of `pyproject.toml`. A run of one example directory takes its rootdir and
`pytest.ini` from the directory, which sets none."""

POLL_SECONDS: Final = 0.05


@dataclass(frozen=True, slots=True)
class Run:
    """One pytest command line of the gate.

    Attributes:
        name: What a failure prints.
        command: The command, run from the project root.
        slots: The pytest processes it starts, xdist workers included.
        proof: Under `--popcorn-part`, the example directories that each need a test that ran on
            the part, empty otherwise.
    """

    name: str
    command: list[str]
    slots: int = 1
    proof: tuple[str, ...] = ()


def example_dirs() -> list[str]:
    """Every example directory, relative to the project root, sorted."""
    return sorted(
        str(ini.parent.relative_to(project_dir))
        for ini in (project_dir / "examples").glob("*/*/pytest.ini")
    )


def part_runs() -> dict[str, list[str]]:
    """The example directories that list each extra part, in table order."""
    by_part: dict[str, list[str]] = {}
    for path, part in extra_runs():
        by_part.setdefault(part, []).append(path)
    return by_part


def _pytest(*args: str) -> list[str]:
    return [sys.executable, "-m", "pytest", "-q", *args]


def collect_commands() -> list[Run]:
    """One collect-only run per example directory."""
    return [Run(path, _pytest("--collect-only", path)) for path in example_dirs()]


def part_commands(jobs: int, stop: list[str] | None = None) -> list[Run]:
    """One run per extra part, the wide ones first. No run starts more than @p jobs processes,
    and with @p jobs 1 every run is serial."""
    runs: list[tuple[int, Run]] = []
    for part, paths in part_runs().items():
        wide = max((WIDE_EXAMPLES.get(path, 0) for path in paths), default=0)
        workers = 0 if jobs <= 1 else min(wide, jobs)
        command = _pytest(
            *(stop or []),
            *TEST_TIMEOUT_ARGS,
            "-n",
            str(workers),
            f"--popcorn-part={part}",
            *paths,
        )
        runs.append((workers, Run(part, command, max(workers, 1), tuple(paths))))
    runs.sort(key=lambda item: -item[0])
    return [run for _, run in runs]


@dataclass(slots=True)
class _Active:
    run: Run
    process: subprocess.Popen[bytes]
    output: IO[bytes]
    started: float
    ids: Path | None


def _kill(active: _Active) -> None:
    with contextlib.suppress(ProcessLookupError):
        os.killpg(active.process.pid, signal.SIGKILL)
    active.process.wait()


def unproven(run: Run, ids: Path) -> list[str]:
    """The directories of @p run proof that no test of @p ids counted, all when it is unreadable."""
    try:
        ran = json.loads(ids.read_text())
    except OSError, ValueError:
        return list(run.proof)
    roots = [str((project_dir / path).resolve()) + os.sep for path in run.proof]
    return [
        path
        for path, root in zip(run.proof, roots, strict=True)
        if not any(name.startswith(root) for name in ran)
    ]


def _verdict(active: _Active, code: int | None) -> str | None:
    if code is None:
        return f"TIMEOUT after {RUN_TIMEOUT:g} s: {active.run.name}"
    if code != 0:
        return f"FAILED ({code}): {active.run.name}"
    if active.ids is not None:
        missing = unproven(active.run, active.ids)
        if missing:
            return f"NO TEST RAN ON THE PART in {', '.join(missing)}: {active.run.name}"
    return None


def run_all(runs: list[Run], jobs: int, fail_fast: bool = False) -> int:
    """Run @p runs, at most @p jobs processes at a time, and return 0 when every one passes.

    A run holds `slots` of the @p jobs. A failure prints its output when the run ends. With
    @p fail_fast the first failure kills the running ones and starts no queued one. An interrupt
    kills every running process group and starts none, as do SIGTERM and SIGHUP. A signal sets a
    flag that the loop reads between two steps, so no spawned group goes unlisted, and the stage
    returns 128 plus the signal number. Must run on the main thread, where it installs the
    handlers of SIGINT, SIGTERM and SIGHUP and restores them on return.
    """
    stopping = 0

    def stop(signum: int, frame: FrameType | None) -> None:
        nonlocal stopping
        stopping = stopping or signum

    previous = {
        sig: signal.signal(sig, stop) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }
    pending = deque(runs)
    running: list[_Active] = []
    failed = 0
    done = 0
    with tempfile.TemporaryDirectory(prefix="example-runs-") as scratch:
        try:
            while (pending or running) and not stopping:
                used = sum(item.run.slots for item in running)
                while pending and not stopping and used + min(pending[0].slots, jobs) <= jobs:
                    run = pending.popleft()
                    used += min(run.slots, jobs)
                    ids: Path | None = None
                    command = run.command
                    if run.proof:
                        ids = Path(scratch) / f"{time.monotonic_ns()}.json"
                        command = [
                            *command,
                            "-p",
                            "popcorn_build.part_ids",
                            "--part-ids-file",
                            str(ids),
                        ]
                    output = tempfile.TemporaryFile()  # noqa: SIM115
                    process = subprocess.Popen(
                        command,
                        cwd=project_dir,
                        stdout=output,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                    running.append(_Active(run, process, output, time.monotonic(), ids))
                for item in list(running):
                    code = item.process.poll()
                    timed_out = code is None and time.monotonic() - item.started > RUN_TIMEOUT
                    if code is None and not timed_out:
                        continue
                    if timed_out:
                        _kill(item)
                        code = None
                    running.remove(item)
                    done += 1
                    verdict = _verdict(item, code)
                    if verdict is not None:
                        failed += 1
                        item.output.seek(0)
                        click.echo(verdict)
                        click.echo(item.output.read().decode(errors="replace"))
                    item.output.close()
                if failed and fail_fast:
                    for item in running:
                        _kill(item)
                        item.output.close()
                    running.clear()
                    pending.clear()
                time.sleep(POLL_SECONDS)
        finally:
            for item in running:
                _kill(item)
                item.output.close()
            for sig, handler in previous.items():
                signal.signal(sig, handler)
    if stopping:
        return 128 + stopping
    click.echo(f"{done - failed} of {len(runs)} runs passed")
    return 1 if failed else 0


@click.group()
def main() -> None:
    pass


@main.command("collect")
@click.option("--jobs", type=click.IntRange(min=1), default=1, help="Processes at a time.")
@click.option("--fail-fast", is_flag=True, help="Stop at the first failing run.")
def collect_command(jobs: int, fail_fast: bool) -> None:
    raise SystemExit(run_all(collect_commands(), jobs, fail_fast))


@main.command("parts")
@click.option("--jobs", type=click.IntRange(min=1), default=1, help="Processes at a time.")
@click.option("--fail-fast", is_flag=True, help="Stop at the first failing run.")
def parts_command(jobs: int, fail_fast: bool) -> None:
    raise SystemExit(run_all(part_commands(jobs, ["-x"] if fail_fast else []), jobs, fail_fast))


if __name__ == "__main__":
    main()

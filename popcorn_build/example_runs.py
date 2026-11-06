"""The example runs of the gate, each in its own pytest process, several at a time.

`python -m popcorn_build.example_runs collect --jobs N` runs `pytest --collect-only` once per
`examples/<chip>/<name>` directory with a `pytest.ini`, which proves each directory runs
standalone from its own rootdir. A directory that collects no test fails.
At most N processes run at a time. The output of each failing process is printed.
"""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor

import click

from popcorn_build.config import project_dir


def example_dirs() -> list[str]:
    """Every example directory, relative to the project root, sorted."""
    return sorted(
        str(ini.parent.relative_to(project_dir))
        for ini in (project_dir / "examples").glob("*/*/pytest.ini")
    )


def _pytest(*args: str) -> list[str]:
    return [sys.executable, "-m", "pytest", "-q", *args]


def collect_commands() -> list[tuple[str, list[str]]]:
    """One collect-only command per example directory."""
    return [(path, _pytest("--collect-only", path)) for path in example_dirs()]


def run_all(commands: Sequence[tuple[str, list[str]]], jobs: int) -> int:
    """Run @p commands, at most @p jobs at a time, and return 0 when every one exits 0."""

    def one(item: tuple[str, list[str]]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(item[1], cwd=project_dir, capture_output=True, text=True, check=False)

    with ThreadPoolExecutor(max_workers=max(jobs, 1)) as pool:
        results = list(pool.map(one, commands))
    failed = 0
    for (name, _), result in zip(commands, results, strict=True):
        if result.returncode != 0:
            failed += 1
            click.echo(f"FAILED ({result.returncode}): {name}")
            click.echo(result.stdout + result.stderr)
    click.echo(f"{len(commands) - failed} of {len(commands)} runs passed")
    return 1 if failed else 0


@click.group()
def main() -> None:
    pass


@main.command("collect")
@click.option("--jobs", type=click.IntRange(min=1), default=1, help="Processes at a time.")
def collect_command(jobs: int) -> None:
    raise SystemExit(run_all(collect_commands(), jobs))


if __name__ == "__main__":
    main()

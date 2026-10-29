import shutil
import subprocess
import sys
import time

import click
from pcons import cli_command

from popcorn_build.config import project_dir
from popcorn_build.example_parts import EXAMPLE_PARTS

Stage = tuple[str, list[str]]


def _pcons(*args: str) -> list[str]:
    return [sys.executable, "-m", "pcons", *args]


def _python_tool(tool: str, *args: str) -> list[str]:
    return [sys.executable, "-m", tool, *args]


def _native_sources() -> list[str]:
    listed = subprocess.run(
        ["git", "ls-files", "*.c", "*.h", "*.cpp", "*.hpp"],
        cwd=project_dir,
        capture_output=True,
        text=True,
        check=True,
    )
    return listed.stdout.split()


def _clang_format() -> list[str]:
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        raise SystemExit("popcorn check needs clang-format on PATH, found none")
    return [clang_format, "--dry-run", "-Werror", *_native_sources()]


def _examples() -> list[Stage]:
    examples = sorted(ini.parent for ini in (project_dir / "examples").glob("*/*/pytest.ini"))
    stages: list[Stage] = []
    for example in examples:
        name = f"{example.parent.name}/{example.name}"
        path = str(example.relative_to(project_dir))
        stages.append((name, _python_tool("pytest", "-q", path)))
        stages.extend(
            (f"{name}@{part}", _python_tool("pytest", "-q", f"--popcorn-part={part}", path))
            for part in EXAMPLE_PARTS[name].extra
        )
    return stages


GATE_VARIABLES = ("POPCORN_BUILD_TESTS=ON", "POPCORN_BUILD_EXAMPLES=ON")


EXAMPLE_WORKERS = 4


def _pcons_jobs(jobs: int | None) -> list[str]:
    return [] if jobs is None else ["-j", str(jobs)]


def _xdist(jobs: int | None, cap: int | None = None) -> list[str]:
    if jobs is not None and jobs <= 1:
        return ["-n", "0"]
    if cap is not None:
        return ["-n", str(cap if jobs is None else min(jobs, cap))]
    return ["-n", "auto" if jobs is None else str(jobs)]


def stages(asan: bool, jobs: int | None = None) -> list[Stage]:
    """The gate, in order. With @p asan, the native suite in build-asan only.

    The build stages set the test and example switches ON: a package build persists them OFF in
    the same build directory. @p jobs caps every parallel stage, None leaves each at its default."""
    if asan:
        return [
            (
                "build",
                _pcons("-B", "build-asan", "--variant=asan", *_pcons_jobs(jobs), *GATE_VARIABLES),
            ),
            ("test", _pcons("-B", "build-asan", "test", *_pcons_jobs(jobs))),
        ]
    return [
        ("build", _pcons("-B", "build", *_pcons_jobs(jobs), *GATE_VARIABLES)),
        ("test", _pcons("-B", "build", "test", *_pcons_jobs(jobs))),
        ("pytest", _python_tool("pytest", "-q", *_xdist(jobs), "tests/python", "tests/docs")),
        *_examples(),
        (
            "examples",
            _python_tool("pytest", "-q", *_xdist(jobs, EXAMPLE_WORKERS), "examples"),
        ),
        ("docs", _python_tool("popcorn_build.docs")),
        ("pyright", _python_tool("pyright")),
        ("ruff", _python_tool("ruff", "check", ".")),
        ("black", _python_tool("black", "--check", "-q", ".")),
        ("clang-format", _clang_format()),
        ("layering", _python_tool("popcorn_build.layering")),
    ]


def run(asan: bool, jobs: int | None = None) -> int:
    summary: list[str] = []
    status = 0
    for name, command in stages(asan, jobs):
        start = time.monotonic()
        code = subprocess.run(command, cwd=project_dir, check=False).returncode
        summary.append(
            f"check: {name:<12} {'ok' if code == 0 else 'FAILED'} {time.monotonic() - start:6.1f} s"
        )
        if code != 0:
            status = code
            break
    click.echo("\n".join(summary))
    return status


def register() -> None:
    @cli_command()
    @click.option(
        "--asan", is_flag=True, help="Run the native suite in the asan variant, build-asan."
    )
    @click.option(
        "-j",
        "--jobs",
        type=click.IntRange(min=1),
        help="Cap the build and every test stage at this many jobs, 1 runs the tests serially. "
        "Default: all cores, examples at 4 workers.",
    )
    def check(asan: bool, jobs: int | None) -> None:
        """Build, test, type check and lint. Stops at the first failing stage."""
        status = run(asan, jobs)
        if status != 0:
            raise SystemExit(status)

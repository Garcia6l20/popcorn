import shutil
import subprocess
import sys
import time

import click
from pcons import cli_command

from popcorn_build.config import project_dir

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


GATE_VARIABLES = ("POPCORN_BUILD_TESTS=ON", "POPCORN_BUILD_EXAMPLES=ON")


DEFAULT_JOBS = 8


def _pcons_jobs(jobs: int) -> list[str]:
    return ["-j", str(jobs)]


def _xdist(jobs: int) -> list[str]:
    return ["-n", "0" if jobs <= 1 else str(jobs)]


def stages(asan: bool, jobs: int | None = None, fail_fast: bool = False) -> list[Stage]:
    """The gate, in order. With @p asan, the native suite in build-asan only.

    The docs stage builds `site/` before the pytest stage, whose `tests/docs` read it.

    The build stages set the test and example switches ON: a package build persists them OFF in
    the same build directory. @p jobs sets the job count of every parallel stage, build, native
    tests, pytest, examples and the example collection, `DEFAULT_JOBS` when None. 1 runs the tests
    serially. The examples stage runs every example on its own part, `examples-collect` collects
    each example directory in its own process to prove it runs standalone, and `examples-parts`
    runs one pytest per extra part over the examples listing that part, @p jobs at a time.
    With @p fail_fast, each test stage stops at its first failing test."""
    jobs = DEFAULT_JOBS if jobs is None else jobs
    native_stop = ["--stop-on-fail"] if fail_fast else []
    pytest_stop = ["-x"] if fail_fast else []
    runs_stop = ["--fail-fast"] if fail_fast else []
    if asan:
        return [
            (
                "build",
                _pcons("-B", "build-asan", "--variant=asan", *_pcons_jobs(jobs), *GATE_VARIABLES),
            ),
            ("test", _pcons("-B", "build-asan", "test", *_pcons_jobs(jobs), *native_stop)),
        ]
    return [
        ("build", _pcons("-B", "build", *_pcons_jobs(jobs), *GATE_VARIABLES)),
        ("test", _pcons("-B", "build", "test", *_pcons_jobs(jobs), *native_stop)),
        ("docs", _python_tool("popcorn_build.docs")),
        (
            "pytest",
            _python_tool("pytest", "-q", *pytest_stop, *_xdist(jobs), "tests/python", "tests/docs"),
        ),
        ("examples", _python_tool("pytest", "-q", *pytest_stop, *_xdist(jobs), "examples")),
        (
            "examples-collect",
            _python_tool("popcorn_build.example_runs", "collect", "--jobs", str(jobs), *runs_stop),
        ),
        (
            "examples-parts",
            _python_tool("popcorn_build.example_runs", "parts", "--jobs", str(jobs), *runs_stop),
        ),
        ("pyright", _python_tool("pyright")),
        ("ruff", _python_tool("ruff", "check", ".")),
        ("black", _python_tool("black", "--check", "-q", ".")),
        ("clang-format", _clang_format()),
        ("layering", _python_tool("popcorn_build.layering")),
    ]


def run(asan: bool, jobs: int | None = None, fail_fast: bool = False) -> int:
    summary: list[str] = []
    status = 0
    for name, command in stages(asan, jobs, fail_fast):
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
        help="Jobs of the build and of every test stage, 1 runs the tests serially. Default: 8.",
    )
    @click.option(
        "--fail-fast",
        is_flag=True,
        help="Stop each test stage at its first failing test, for iterating. Leave it off for "
        "the gate of record, which reports every failure.",
    )
    def check(asan: bool, jobs: int | None, fail_fast: bool) -> None:
        """Build, test, type check and lint. Stops at the first failing stage."""
        status = run(asan, jobs, fail_fast)
        if status != 0:
            raise SystemExit(status)

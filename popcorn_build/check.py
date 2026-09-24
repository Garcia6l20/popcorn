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


def stages(asan: bool) -> list[Stage]:
    """The gate, in order. With @p asan, the native suite in build-asan only."""
    if asan:
        return [
            ("build", _pcons("-B", "build-asan", "--variant=asan")),
            ("test", _pcons("-B", "build-asan", "test")),
        ]
    return [
        ("build", _pcons("-B", "build")),
        ("test", _pcons("-B", "build", "test")),
        ("pytest", _python_tool("pytest", "-q", "tests/python")),
        ("pyright", _python_tool("pyright")),
        ("ruff", _python_tool("ruff", "check", ".")),
        ("black", _python_tool("black", "--check", "-q", ".")),
        ("clang-format", _clang_format()),
    ]


def run(asan: bool) -> int:
    summary: list[str] = []
    status = 0
    for name, command in stages(asan):
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
    def check(asan: bool) -> None:
        """Build, test, type check and lint. Stops at the first failing stage."""
        status = run(asan)
        if status != 0:
            raise SystemExit(status)

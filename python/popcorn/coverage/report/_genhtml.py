"""The html report through genhtml."""

from __future__ import annotations

import contextlib
import os
import shutil
import signal
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

BUILTIN_MARKER = ".popcorn-html-report"
REPORT_MARKERS = ("gcov.css", BUILTIN_MARKER)
GENHTML_TIMEOUT = 300.0


@dataclass(frozen=True, slots=True)
class GenhtmlResult:
    """How a genhtml run ended.

    Attributes:
        returncode: The exit status of genhtml.
        stderr: What genhtml wrote to its error stream.
        timed_out: genhtml was killed because it ran past the timeout.
    """

    returncode: int
    stderr: str
    timed_out: bool = False


def find_genhtml() -> str | None:
    """The path of the `genhtml` executable on `PATH`, None when it is not installed."""
    return shutil.which("genhtml")


def _is_report(out: Path) -> bool:
    return (out / "index.html").is_file() and any(
        (out / marker).is_file() for marker in REPORT_MARKERS
    )


def prepare_output(out: Path, *, protect: Sequence[tuple[str, Path]] = ()) -> str | None:
    """Make `out` ready for a report, or return why it is not.

    A missing or empty directory is used as is. A directory holding an earlier html report,
    an `index.html` next to `gcov.css` from genhtml or `BUILTIN_MARKER` from the built-in
    renderer, is emptied. Anything else is left untouched: a file, a symbolic link, a directory
    of other content, a report directory that holds a `.git` entry, and a directory that is or
    contains one of `protect`.

    Args:
        out: The directory a report is about to be written to.
        protect: A label and a path for each location `out` must neither be nor contain.

    Returns:
        None when `out` can take a report, else the reason it cannot.
    """
    if out.is_symlink():
        return f"{out} is a symbolic link"
    if not out.exists():
        return None
    if not out.is_dir():
        return f"{out} is not a directory"
    resolved = out.resolve()
    for label, path in protect:
        held = path.resolve()
        if held == resolved or resolved in held.parents:
            return f"{out} holds {label}"
    if _is_report(out):
        if os.path.lexists(out / ".git"):
            return f"{out} holds a .git entry"
        shutil.rmtree(out)
        return None
    if any(out.iterdir()):
        return f"{out} is not empty and holds no earlier html report"
    return None


def run_genhtml(
    lcov: Path, out: Path, *, root: Path, timeout: float | None = None
) -> GenhtmlResult:
    """Render `lcov` into the directory `out` with branch and function coverage.

    genhtml runs with `root` as its working directory, where a relative `SF:` path of the
    tracefile resolves. A source it cannot read renders as a page of blank lines. The one
    consistency error the lcov export can legitimately produce is ignored. The options need
    genhtml from LCOV 2.0 or later. A genhtml still running after `timeout` seconds,
    `GENHTML_TIMEOUT` when None, is killed with every process of its group, and the result has
    `timed_out` set.

    Raises:
        FileNotFoundError: genhtml is not installed.
    """
    genhtml = find_genhtml()
    if genhtml is None:
        raise FileNotFoundError("genhtml is not installed")
    limit = GENHTML_TIMEOUT if timeout is None else timeout
    command = [
        genhtml,
        "--quiet",
        "--branch-coverage",
        "--function-coverage",
        "--ignore-errors",
        "inconsistent,source",
        "--synthesize-missing",
        "-o",
        str(out.absolute()),
        str(lcov.absolute()),
    ]
    with subprocess.Popen(
        command,
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    ) as process:
        try:
            _, stderr = process.communicate(timeout=limit)
        except BaseException as error:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            if isinstance(error, subprocess.TimeoutExpired):
                return GenhtmlResult(-1, "", timed_out=True)
            raise
    return GenhtmlResult(process.returncode, stderr)

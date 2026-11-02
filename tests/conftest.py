"""Turn a pytest-timeout kill of an xdist worker into a timeout failure naming the test."""

import hashlib
import shutil
import sys
import tempfile
import threading
import traceback
from pathlib import Path
from typing import Any

import pytest
from pytest_timeout import (  # pyright: ignore[reportMissingTypeStubs]
    is_debugging,  # pyright: ignore[reportUnknownVariableType]
    timeout_timer,  # pyright: ignore[reportUnknownVariableType]
)

_DIR_KEY = "popcorn_timeout_dir"
_controller_dir: Path | None = None


def _report_path(directory: Path, nodeid: str) -> Path:
    return directory / hashlib.sha256(nodeid.encode()).hexdigest()


def _stacks() -> str:
    names = {thread.ident: thread.name for thread in threading.enumerate()}
    lines: list[str] = []
    for ident, frame in sys._current_frames().items():  # pyright: ignore[reportPrivateUsage]
        if ident == threading.get_ident():
            continue
        lines.append(f"\n--- thread {names.get(ident, ident)} ---\n")
        lines.extend(traceback.format_stack(frame))
    return "".join(lines)


def _expire(item: pytest.Item, settings: Any, directory: Path) -> None:
    if settings.disable_debugger_detection or not is_debugging():
        _report_path(directory, item.nodeid).write_text(
            f"Timeout: {item.nodeid} ran past {settings.timeout:g} s, pytest-timeout killed the"
            f" worker\n{_stacks()}"
        )
    timeout_timer(item, settings)


def pytest_unconfigure() -> None:
    if _controller_dir is not None:
        shutil.rmtree(_controller_dir, ignore_errors=True)


@pytest.hookimpl(optionalhook=True)
def pytest_configure_node(node: Any) -> None:
    global _controller_dir
    if _controller_dir is None:
        _controller_dir = Path(tempfile.mkdtemp(prefix="popcorn-timeouts-"))
    node.workerinput[_DIR_KEY] = str(_controller_dir)


@pytest.hookimpl(tryfirst=True)
def pytest_timeout_set_timer(item: pytest.Item, settings: Any) -> bool | None:
    workerinput = getattr(item.config, "workerinput", None)
    if workerinput is None or _DIR_KEY not in workerinput or settings.method != "thread":
        return None
    timer = threading.Timer(
        settings.timeout, _expire, (item, settings, Path(workerinput[_DIR_KEY]))
    )
    timer.name = f"popcorn timeout {item.nodeid}"

    def cancel() -> None:
        timer.cancel()
        timer.join()

    item.cancel_timeout = cancel  # pyright: ignore[reportAttributeAccessIssue]
    timer.start()
    return True


@pytest.hookimpl(optionalhook=True)
def pytest_handlecrashitem(crashitem: str, report: pytest.TestReport) -> None:
    if _controller_dir is None:
        return
    path = _report_path(_controller_dir, crashitem)
    if path.exists():
        report.longrepr = path.read_text()

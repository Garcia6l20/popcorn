"""Inputs of the plugin tests: the core test description, the generic firmware, inner files.

Imports popcorn in the outer process: pytester drops the modules an inner run imported, and
the native extension cannot be initialized a second time in one process.
"""

from __future__ import annotations

import json
import textwrap
from dataclasses import dataclass
from pathlib import Path

import popcorn as popcorn
import pytest

ROOT = Path(__file__).parents[3]
M33_JSON = ROOT / "tests" / "data" / "core" / "m33.json"
PREBUILT = ROOT / "tests" / "firmware" / "api" / "prebuilt"
DRIVER_ELF = PREBUILT / "driver.elf"
CALLEE_ELF = PREBUILT / "callee.elf"
FAULT_ELF = PREBUILT / "fault.elf"
BOARD_ELF = PREBUILT / "board.elf"
MODEL_ELF = PREBUILT / "model.elf"
MODEL_JSON = ROOT / "tests" / "data" / "api" / "model.json"

CONFTEST = 'pytest_plugins = ["popcorn.pytest"]\n'

RECORD_LOOPS = """
import json
from pathlib import Path

import pytest

LOOPS = {}
STATE = {}


def record(name, loop):
    LOOPS[name] = loop


def pytest_sessionfinish(session):
    from popcorn.pytest._loop import OWNED_LOOP

    owned = session.config.stash.get(OWNED_LOOP, None)
    STATE["started"] = owned is not None and owned.started


@pytest.hookimpl(trylast=True)
def pytest_unconfigure(config):
    Path("loops.json").write_text(
        json.dumps(
            {
                "ids": {name: id(loop) for name, loop in LOOPS.items()},
                "closed": {name: loop.is_closed() for name, loop in LOOPS.items()},
                "started": STATE["started"],
            }
        )
    )
"""


def _merge(path: Path, before: str, after: str) -> Path:
    """`path` written as `before`, its current content, then `after`."""
    current = path.read_text() if path.is_file() else ""
    path.write_text(f"{before}\n{current}\n{after}")
    return path


def write_conftest(pytester: pytest.Pytester, extra: str = "") -> Path:
    """The inner root conftest loading the plugin, followed by `extra`, before any project
    `write_project` put there."""
    return _merge(pytester.path / "conftest.py", CONFTEST + textwrap.dedent(extra), "")


PROJECT_FIXTURE = """
@pytest.fixture(scope="session")
def description():
    return PROJECT


@popcorn.pytest.machine
def machine():
    return PROJECT
"""


def write_project_conftest(directory: Path, project: str, imports: str = "") -> Path:
    """The `conftest.py` of `directory` defining `PROJECT` as the Python expression `project`,
    after `imports`, appended to what the file holds, with the plain fixture `description`
    returning it, for `machine_factory`, and the machine fixture `machine` declared over it."""
    text = (
        "from pathlib import Path\n\nimport popcorn\nimport popcorn.pytest\nimport pytest\n"
        f"{textwrap.dedent(imports)}\n\nPROJECT = {project}\n{PROJECT_FIXTURE}"
    )
    return _merge(directory / "conftest.py", "", text)


def write_project(
    pytester: pytest.Pytester,
    until: str | None = "alu_mark",
    extra: str = "",
    *,
    vtor: str | None = None,
    directory: Path | None = None,
    elf: Path = DRIVER_ELF,
    configs: str = "",
    imports: str = "",
) -> Path:
    """A `conftest.py` as `write_project_conftest` writes it, over the M33 test description and
    `elf`, `driver.elf` by default, in `directory` or the pytester directory.

    `vtor` names a `vt_*` table of `elf` the boot starts from. `extra` holds more
    `MachineDescription` keyword arguments as Python source, `configs` the keyword arguments of
    `with_configs`, where `BASE` names the project before them, and `imports` the lines its
    expressions need.
    """
    boot = [] if until is None else [f"until={until!r}"]
    if vtor is not None:
        boot.append(f"vtor={popcorn.Image.elf(elf).lookup(vtor).value:#x}")
    base = (
        "popcorn.MachineDescription(\n"
        '    part="POPCORN-M33-TEST",\n'
        f"    description=Path({str(M33_JSON)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(elf)!r}))],\n"
        f"    boot=popcorn.Boot({', '.join(boot)}),\n"
        f"    {extra}\n"
        ")"
    )
    project = f"BASE.with_configs({configs})" if configs else "BASE"
    return write_project_conftest(
        directory or pytester.path, project, f"{imports}\n\nBASE = {base}\n"
    )


def write_model_project(pytester: pytest.Pytester, extra: str = "") -> Path:
    """A `conftest.py` as `write_project_conftest` writes it, over the model test description and
    `model.elf`, booted to `ready`, lax about unmodelled peripherals. `extra` holds more
    `MachineDescription` keyword arguments as Python source."""
    return write_project_conftest(
        pytester.path,
        "popcorn.MachineDescription(\n"
        '    part="POPCORN-M33-TEST",\n'
        f"    description=Path({str(MODEL_JSON)!r}),\n"
        f"    images=[popcorn.Elf(Path({str(MODEL_ELF)!r}))],\n"
        '    boot=popcorn.Boot(until="ready", budget=10_000),\n'
        "    strict=False,\n"
        f"    {extra}\n"
        ")",
    )


@dataclass(frozen=True, slots=True)
class Loops:
    """What an inner run using `RECORD_LOOPS` recorded."""

    ids: dict[str, int]
    closed: dict[str, bool]
    started: bool


def read_loops(pytester: pytest.Pytester) -> Loops:
    """The loops the inner `record` calls saw, and whether popcorn's runner was created."""
    data = json.loads((pytester.path / "loops.json").read_text())
    return Loops(ids=data["ids"], closed=data["closed"], started=data["started"])


def cov_args(path: str) -> tuple[str, ...]:
    """The options that count coverage and write the lcov file `path`."""
    return ("--popcorn-cov", f"--popcorn-cov-report=lcov:{path}")

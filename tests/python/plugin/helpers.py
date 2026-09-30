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


def write_conftest(pytester: pytest.Pytester, extra: str = "") -> Path:
    """The inner root conftest loading the plugin, followed by `extra`."""
    return pytester.makeconftest(CONFTEST + textwrap.dedent(extra))


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


def write_project(
    pytester: pytest.Pytester,
    until: str | None = "alu_mark",
    extra: str = "",
    *,
    vtor: str | None = None,
    directory: Path | None = None,
    elf: Path = DRIVER_ELF,
) -> Path:
    """A `popcorn.toml` over the M33 test description and `elf`, `driver.elf` by default, in
    `directory` or the pytester directory. `vtor` names a `vt_*` table of `elf` the boot starts
    from."""
    boot = "" if until is None else f'until = "{until}"\n'
    if vtor is not None:
        boot += f"vtor = {popcorn.Image.elf(elf).lookup(vtor).value:#x}\n"
    text = (
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{elf}"\n'
        f"{'[boot]' + chr(10) + boot if boot else ''}{textwrap.dedent(extra)}"
    )
    path = (directory or pytester.path) / "popcorn.toml"
    path.write_text(text)
    return path


def write_model_project(pytester: pytest.Pytester, extra: str = "") -> Path:
    """A `popcorn.toml` over the model test description and `model.elf`, booted to `ready`, lax
    about unmodelled peripherals."""
    path = pytester.path / "popcorn.toml"
    path.write_text(
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{MODEL_JSON}"\n'
        f'[image.app]\nelf = "{MODEL_ELF}"\n'
        '[boot]\nuntil = "ready"\nbudget = 10_000\n'
        f"[models]\nstrict = false\n{textwrap.dedent(extra)}"
    )
    return path

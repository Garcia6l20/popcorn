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
    pytester: pytest.Pytester, until: str | None = "alu_mark", extra: str = ""
) -> Path:
    """An inner `popcorn.toml` over the M33 test description and `driver.elf`."""
    boot = f'[boot]\nuntil = "{until}"\n' if until is not None else ""
    text = (
        f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
        f'[image.app]\nelf = "{DRIVER_ELF}"\n'
        f"{boot}{textwrap.dedent(extra)}"
    )
    path = pytester.path / "popcorn.toml"
    path.write_text(text)
    return path

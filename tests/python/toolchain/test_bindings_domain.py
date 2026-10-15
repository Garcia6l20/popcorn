"""`popcorn/plugin/bindings.hpp` refuses to compile in another nanobind domain than popcorn's."""

from __future__ import annotations

import json
import shlex
import subprocess
from pathlib import Path

import pytest
from popcorn import _core

MESSAGE = "a popcorn plugin builds in the nanobind domain popcorn was built in"


def syntax_check(*defines: str) -> subprocess.CompletedProcess[str]:
    """The compile of bindings_check.cpp in compile_commands.json, syntax only, with @p defines."""
    commands = Path(_core.__file__).resolve().parents[1] / "compile_commands.json"
    if not commands.is_file():
        pytest.skip(f"no {commands}")
    entries = json.loads(commands.read_text())
    entry = next(
        (item for item in entries if item["file"].endswith("plugin/bindings_check.cpp")), None
    )
    if entry is None:
        pytest.skip("bindings_check.cpp not in this build")
    argv = shlex.split(entry["command"])
    kept: list[str] = []
    skip = False
    for argument in argv:
        if skip:
            skip = False
        elif argument in ("-o", "-MF"):
            skip = True
        elif argument not in ("-c", "-MD"):
            kept.append(argument)
    return subprocess.run(
        [*kept, "-fsyntax-only", *defines],
        cwd=entry["directory"],
        capture_output=True,
        text=True,
    )


def test_the_header_compiles_in_popcorns_domain() -> None:
    ran = syntax_check()
    assert ran.returncode == 0, ran.stderr


def test_the_header_refuses_another_domain() -> None:
    ran = syntax_check("-DNB_DOMAIN=other")
    assert ran.returncode != 0
    assert MESSAGE in ran.stderr

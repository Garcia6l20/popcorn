"""genhtml 2 messages for function shapes real firmware has, from `--popcorn-cov` sessions."""

from __future__ import annotations

import subprocess

import pytest

from .helpers import ROOT
from .test_plugin_genhtml import GENHTML, genhtml, messages, session

pytest_plugins = ["pytester"]

SHAPES_ELF = ROOT / "tests" / "firmware" / "obs" / "prebuilt" / "genhtml_shapes.elf"

BOOT_ONLY = """
async def test_booted(machine):
    assert machine.pc == machine.sym("shapes_done")
"""

INLINE_AFTER = (
    '"tests/firmware/obs/genhtml_shapes.c":15: '
    "function 'inline_f' is not hit but line 22 is: function marked 'hit'."
)


def shape_messages(rendered: subprocess.CompletedProcess[str]) -> list[str]:
    """The genhtml messages about the shapes source, genhtml's prefix and suffix stripped."""
    found: list[str] = []
    for line in messages(rendered):
        if "genhtml_shapes.c" not in line:
            continue
        text = line.split(") ", 1)[1] if ") " in line else line
        found.append(text.split(" (use ", 1)[0])
    return found


@pytest.mark.skipif(GENHTML is None, reason="genhtml is not installed")
def test_the_shapes_elf_gives_only_the_inline_after_message(pytester: pytest.Pytester) -> None:
    lcov = session(pytester, SHAPES_ELF, "shapes_done", BOOT_ONLY, "shapes", "-p", "no:xdist")
    rendered = genhtml(lcov, pytester.path / "html", "--ignore-errors", "inconsistent")
    assert rendered.returncode == 0, rendered.stderr
    assert shape_messages(rendered) == [INLINE_AFTER], messages(rendered)

"""NOTICE names the Unicorn fork commit the repository pins as its submodule."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


def pinned_unicorn_commit() -> str:
    """The gitlink of third_party/unicorn in the index, which a staged submodule bump updates."""
    if shutil.which("git") is None or not (ROOT / ".git").exists():
        pytest.skip("not a git checkout")
    entry = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "--stage", "--", "third_party/unicorn"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    assert entry[0] == "160000", entry
    return entry[1]


def notice_unicorn_commit() -> str:
    text = (ROOT / "NOTICE").read_text()
    match = re.search(r"Unicorn CPU emulator, popcorn fork at commit\s+([0-9a-f]{40}),", text)
    assert match is not None, "NOTICE has no Unicorn fork commit line"
    return match.group(1)


def test_notice_names_the_pinned_unicorn_commit() -> None:
    assert notice_unicorn_commit() == pinned_unicorn_commit()

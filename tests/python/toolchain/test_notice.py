"""NOTICE and the image notice derive the Unicorn source from the pinned submodule."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
UPSTREAM_COMMIT = re.compile(r"Upstream: \S+, commit\s+([0-9a-f]{40}),")
HEX_RUN = re.compile(r"\b(?=[0-9a-f]*[0-9])(?=[0-9a-f]*[a-f])[0-9a-f]{7,40}\b")
ENTRY_DATE = re.compile(r"^  (\d{4}-\d{2}-\d{2}), ", re.MULTILINE)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout.strip()


def pinned_unicorn_commit() -> str:
    """The gitlink of third_party/unicorn in the index, which a staged submodule bump updates."""
    if shutil.which("git") is None or not (ROOT / ".git").exists():
        pytest.skip("not a git checkout")
    entry = git("-C", str(ROOT), "ls-files", "--stage", "--", "third_party/unicorn").split()
    assert entry and entry[0] == "160000", entry
    return entry[1]


def unicorn_block() -> str:
    text = (ROOT / "NOTICE").read_text()
    start = text.index("Unicorn CPU emulator")
    return text[start : text.index("License: GPL-2.0-only", start)]


def flat(text: str) -> str:
    return " ".join(text.split())


def test_notice_names_only_the_upstream_base_commit_for_unicorn() -> None:
    block = unicorn_block()
    upstream = UPSTREAM_COMMIT.findall(block)
    assert len(upstream) == 1, upstream
    assert HEX_RUN.findall(block) == upstream


def test_notice_derives_the_fork_commit_from_the_submodule() -> None:
    assert (
        "The fork commit is the one the third_party/unicorn submodule of this popcorn commit pins."
        in flat(unicorn_block())
    )


def test_image_notice_derives_the_unicorn_source_from_the_revision_label() -> None:
    text = flat((ROOT / "docker" / "IMAGE_NOTICE").read_text())
    assert "org.opencontainers.image.revision" in text
    assert (
        "The Unicorn source is the fork commit that the third_party/unicorn submodule of that "
        "revision pins, from https://github.com/Garcia6l20/unicorn, branch popcorn." in text
    )


def test_notice_change_list_reaches_the_pinned_commit() -> None:
    """The newest dated entry is not older than the pinned fork commit."""
    commit = pinned_unicorn_commit()
    submodule = ROOT / "third_party" / "unicorn"
    if not (submodule / ".git").exists():
        pytest.skip("third_party/unicorn is not initialised")
    present = subprocess.run(
        ["git", "-C", str(submodule), "cat-file", "-e", f"{commit}^{{commit}}"],
        capture_output=True,
    )
    if present.returncode != 0:
        pytest.skip(f"third_party/unicorn does not hold the pinned commit {commit}")
    pinned = git("-C", str(submodule), "log", "-1", "--format=%as", commit)
    dates = ENTRY_DATE.findall(unicorn_block())
    assert dates, "NOTICE lists no dated Unicorn change"
    assert max(dates) >= pinned, (max(dates), pinned)

"""Regenerates the golden locate files and checks they match the committed ones."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

from image.golden import FIRMWARE_NAMES, write_golden

REPO_ROOT = Path(__file__).resolve().parents[3]
PREBUILT_DIR = REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt"
DATA_DIR = REPO_ROOT / "tests" / "native" / "image" / "data"

pytestmark = pytest.mark.skipif(
    shutil.which("arm-none-eabi-addr2line") is None,
    reason="arm-none-eabi-addr2line not on PATH",
)


@pytest.mark.parametrize("name", FIRMWARE_NAMES)
def test_golden_file_is_fresh(name: str, tmp_path: Path) -> None:
    elf_path = PREBUILT_DIR / f"{name}.elf"
    committed = DATA_DIR / f"locate_{name}.txt"
    regenerated = tmp_path / f"locate_{name}.txt"
    write_golden(elf_path, regenerated)
    assert regenerated.read_text() == committed.read_text()

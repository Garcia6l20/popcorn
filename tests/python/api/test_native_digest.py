"""The native digest covers every popcorn library a session loads, and needs no pytest."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from popcorn import _core
from popcorn._native_digest import native_digest, native_libraries

AREAS = ("armv8m", "core", "devices", "obs", "stm32", "usb")


def test_the_libraries_are_core_and_every_popcorn_library_beside_it() -> None:
    names = [path.name for path in native_libraries()]
    assert names == [Path(str(_core.__file__)).name, *(f"libpopcorn_{area}.so" for area in AREAS)]


@pytest.mark.parametrize("area", AREAS)
def test_a_change_to_one_library_alone_changes_the_digest(tmp_path: Path, area: str) -> None:
    for path in native_libraries():
        shutil.copy2(path, tmp_path / path.name)
    library = tmp_path / f"libpopcorn_{area}.so"
    extension = tmp_path / Path(str(_core.__file__)).name
    before = native_digest(tmp_path)
    extension_bytes = extension.read_bytes()
    data = bytearray(library.read_bytes())
    data[len(data) // 2] ^= 0xFF
    library.write_bytes(bytes(data))
    assert native_digest(tmp_path) != before
    assert extension.read_bytes() == extension_bytes


def test_an_empty_directory_has_an_empty_digest(tmp_path: Path) -> None:
    assert native_libraries(tmp_path) == []
    assert native_digest(tmp_path) == ""


def test_the_digest_module_imports_with_pytest_blocked() -> None:
    code = (
        "import sys\n"
        "sys.modules['pytest'] = None\n"
        "import popcorn._native_digest\n"
        "assert 'popcorn.pytest' not in sys.modules\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stderr

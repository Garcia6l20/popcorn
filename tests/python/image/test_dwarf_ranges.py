"""Checks function addresses read from DW_AT_ranges lists.

data/shared_ranges_units.s builds UNITS units of FUNCTIONS subprograms each, all with
DW_AT_ranges to one list whose lowest start is 0x08000040. data/relative_ranges_dw4.s builds two
version 4 units at 0x08000000 and 0x08001000 sharing one .debug_ranges list of offsets.
data/mixed_ranges.s builds a version 4 and a version 5 unit whose DW_AT_ranges have the same
value in .debug_ranges and .debug_rnglists, and two version 5 units with different address
bases sharing one list of DW_RLE_startx_length entries.
"""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

import pytest
from popcorn.image import load_debug_info

DATA = Path(__file__).resolve().parent / "data"

pytestmark = pytest.mark.skipif(
    shutil.which("arm-none-eabi-as") is None, reason="no arm-none-eabi-as"
)


def _assemble(tmp_path: Path, source: str, symbols: dict[str, int]) -> Path:
    elf = tmp_path / f"{source}.o"
    defsyms = [arg for name, value in symbols.items() for arg in ("--defsym", f"{name}={value}")]
    subprocess.run(["arm-none-eabi-as", *defsyms, "-o", str(elf), str(DATA / source)], check=True)
    return elf


@pytest.mark.parametrize(
    ("units", "functions"), [(400, 1), (1, 3000)], ids=["across units", "within a unit"]
)
def test_shared_range_list_is_read_once(tmp_path: Path, units: int, functions: int) -> None:
    symbols = {"UNITS": units, "FUNCTIONS": functions, "ENTRIES": 3000}
    elf = _assemble(tmp_path, "shared_ranges_units.s", symbols)
    started = time.perf_counter()
    info = load_debug_info(elf)
    elapsed = time.perf_counter() - started
    assert info.functions["f"].address == 0x08000040
    assert elapsed < 1.0


def test_relative_list_shared_by_units_with_different_bases(tmp_path: Path) -> None:
    info = load_debug_info(_assemble(tmp_path, "relative_ranges_dw4.s", {}))
    assert info.functions["first"].address == 0x08000010
    assert info.functions["second"].address == 0x08001010


def test_range_lists_of_both_sections_at_one_offset(tmp_path: Path) -> None:
    info = load_debug_info(_assemble(tmp_path, "mixed_ranges.s", {}))
    assert info.functions["version4"].address == 0x08000400
    assert info.functions["version5"].address == 0x08000800


def test_indexed_list_shared_by_units_with_different_address_bases(tmp_path: Path) -> None:
    info = load_debug_info(_assemble(tmp_path, "mixed_ranges.s", {}))
    assert info.functions["first_base"].address == 0x08000C00
    assert info.functions["second_base"].address == 0x08000E00

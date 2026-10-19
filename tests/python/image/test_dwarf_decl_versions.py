"""Checks declaration files of image.elf, built with plain `-g`: GCC writes version 5 units and
the assembler a version 3 line table, whose file indexes start at 1. data/bad_include_dir.s
builds a version 4 line table whose file entry names a directory past its table.

Expected lines were read from tests/firmware/image/main.c and shapes.c.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from typing import cast

import pytest
from elftools.elf.elffile import ELFFile
from popcorn.image import DebugInfo, Decl, load_debug_info

REPO_ROOT = Path(__file__).resolve().parents[3]
ELF_PATH = REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt" / "image.elf"
MAIN_SOURCE = "tests/firmware/image/main.c"
SHAPES_SOURCE = "tests/firmware/image/shapes.c"


@pytest.fixture(scope="module")
def info() -> DebugInfo:
    return load_debug_info(ELF_PATH)


def test_units_and_line_tables_differ_in_version() -> None:
    with ELF_PATH.open("rb") as stream:
        dwarf_info = ELFFile(stream).get_dwarf_info()
        for cu in dwarf_info.iter_CUs():
            line_program = dwarf_info.line_program_for_CU(cu)
            assert line_program is not None
            assert cast(int, cu.header.version) == 5
            assert cast(int, line_program.header["version"]) == 3


def test_variable_decls(info: DebugInfo) -> None:
    assert info.variables["global_counter"].decl == Decl(file=MAIN_SOURCE, line=10)
    assert info.variables["zero_buffer"].decl == Decl(file=SHAPES_SOURCE, line=10)


def test_function_decls(info: DebugInfo) -> None:
    assert info.functions["scale"].decl == Decl(file=MAIN_SOURCE, line=12)
    assert info.functions["Reset_Handler"].decl == Decl(file=MAIN_SOURCE, line=17)
    assert info.functions["shapes_perimeter"].decl == Decl(file=SHAPES_SOURCE, line=12)


@pytest.mark.skipif(shutil.which("arm-none-eabi-as") is None, reason="no arm-none-eabi-as")
def test_include_dir_index_past_the_table_gives_no_decl(tmp_path: Path) -> None:
    elf = tmp_path / "bad_include_dir.o"
    source = Path(__file__).resolve().parent / "data" / "bad_include_dir.s"
    subprocess.run(["arm-none-eabi-as", "-o", str(elf), str(source)], check=True)
    info = load_debug_info(elf)
    assert info.variables["bad"].address == 0x20000000
    assert info.variables["bad"].decl is None
    assert info.variables["good"].decl == Decl(file="/w/inc/good.c", line=4)

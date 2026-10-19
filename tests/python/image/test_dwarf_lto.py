"""Checks popcorn.image.dwarf against image-lto.elf, two units linked with `-flto`.

GCC writes each file-scope variable of an LTO link twice: a DIE with its name, type and source
line in the unit of its source file, and a DIE in the `<artificial>` ltrans unit holding only
DW_AT_abstract_origin and DW_AT_location. Expected addresses were read with
`arm-none-eabi-nm tests/firmware/image/prebuilt/image-lto.elf`, types with
`gdb -nx -batch -ex "ptype/o struct lto_pair" -ex "whatis <var>"` on the same committed ELF.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from popcorn.image import BaseType, DebugInfo, Decl, StructType, Typedef, load_debug_info

REPO_ROOT = Path(__file__).resolve().parents[3]
ELF_PATH = REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt" / "image-lto.elf"
MAIN_SOURCE = "tests/firmware/image/lto_main.c"
PEER_SOURCE = "tests/firmware/image/lto_peer.c"


@pytest.fixture(scope="module")
def info() -> DebugInfo:
    return load_debug_info(ELF_PATH)


def test_ltrans_unit_holds_the_locations(info: DebugInfo) -> None:
    assert info.units[0].name == "<artificial>"


def test_global_reached_through_origin_and_specification(info: DebugInfo) -> None:
    variable = info.variables["lto_global"]
    assert variable.name == "lto_global"
    assert variable.address == 0x20000014
    assert variable.decl == Decl(file=PEER_SOURCE, line=5)
    pair = info.resolve(variable.type)
    assert isinstance(pair, StructType)
    assert pair.name == "lto_pair"
    assert pair.size == 8
    assert [(m.name, m.offset) for m in pair.members] == [("first", 0), ("second", 4)]
    second = info.strip(pair.members[1].type)
    assert isinstance(second, BaseType)
    assert second.size == 2


def test_static_of_the_main_unit(info: DebugInfo) -> None:
    variable = info.variables["lto_main_count"]
    assert variable.address == 0x20000004
    assert variable.decl == Decl(file=MAIN_SOURCE, line=7)
    typedef = info.resolve(variable.type)
    assert isinstance(typedef, Typedef)
    assert typedef.name == "int32_t"
    base = info.strip(variable.type)
    assert isinstance(base, BaseType)
    assert base.size == 4


def test_static_of_the_peer_unit(info: DebugInfo) -> None:
    variable = info.variables["lto_peer_hits"]
    assert variable.address == 0x20000008
    assert variable.decl == Decl(file=PEER_SOURCE, line=9)
    typedef = info.resolve(variable.type)
    assert isinstance(typedef, Typedef)
    assert typedef.name == "uint8_t"
    base = info.strip(variable.type)
    assert isinstance(base, BaseType)
    assert base.size == 1


def test_definition_line_overrides_the_declaration_line(info: DebugInfo) -> None:
    variable = info.variables["lto_late"]
    assert variable.address == 0x20000010
    assert variable.decl == Decl(file=PEER_SOURCE, line=11)


def test_global_wins_over_a_static_of_the_same_name(info: DebugInfo) -> None:
    variable = info.variables["lto_shared"]
    assert variable.address == 0x2000000C
    assert variable.decl == Decl(file=PEER_SOURCE, line=7)
    typedef = info.resolve(variable.type)
    assert isinstance(typedef, Typedef)
    assert typedef.name == "uint32_t"


def test_function_decl_read_from_its_origin_unit(info: DebugInfo) -> None:
    function = info.functions["Reset_Handler"]
    assert function.address == 0x08000008
    assert function.decl == Decl(file=MAIN_SOURCE, line=11)


def test_inlined_function_read_from_its_definition_not_a_prototype(info: DebugInfo) -> None:
    function = info.functions["lto_bump"]
    assert function.address is None
    assert [param.name for param in function.params] == ["step"]
    assert function.decl == Decl(file=PEER_SOURCE, line=13)
    assert function.returns is not None
    returns = info.resolve(function.returns)
    assert isinstance(returns, Typedef)
    assert returns.name == "uint32_t"

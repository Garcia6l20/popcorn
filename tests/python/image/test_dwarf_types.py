"""Checks popcorn.image.dwarf against image-types.elf, cross-checked with gdb and addr2line.

Expected member offsets, sizes and bit layouts below were read with
`gdb -nx -batch -ex "ptype/o <type>" tests/firmware/image/prebuilt/image-types.elf`
(host /usr/bin/gdb, 2026-09-25). Expected addresses were read with `gdb ... -ex "print &<var>"`
and `-ex "info address <fn>"` against the same committed ELF.
"""

from __future__ import annotations

import gc
import posixpath
import shutil
import subprocess
import threading
from collections.abc import Sequence
from pathlib import Path
from typing import cast

import pytest
from elftools.dwarf.compileunit import CompileUnit
from elftools.dwarf.die import DIE
from elftools.dwarf.ranges import BaseAddressEntry, RangeEntry
from popcorn.image import (
    ArrayType,
    BaseType,
    DebugInfo,
    DebugInfoError,
    Encoding,
    EnumType,
    PointerType,
    Qualified,
    Qualifier,
    StructType,
    Typedef,
    dwarf,
    load_debug_info,
)
from popcorn.image.dwarf import address_from_ranges

REPO_ROOT = Path(__file__).resolve().parents[3]
ELF_PATH = REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt" / "image-types.elf"
SOURCE_FILE = "tests/firmware/image/types.c"


@pytest.fixture(scope="module")
def info() -> DebugInfo:
    return load_debug_info(ELF_PATH)


def test_struct_with_bitfield(info: DebugInfo) -> None:
    flags = info.type("struct flags")
    assert isinstance(flags, StructType)
    assert flags.name == "flags"
    assert flags.size == 4
    assert not flags.union
    by_name = {m.name: m for m in flags.members}
    assert by_name["enabled"].offset == 0
    assert by_name["enabled"].bit_offset == 0
    assert by_name["enabled"].bit_size == 1
    assert by_name["mode"].offset == 0
    assert by_name["mode"].bit_offset == 1
    assert by_name["mode"].bit_size == 3
    assert by_name["reserved"].offset == 0
    assert by_name["reserved"].bit_offset == 4
    assert by_name["reserved"].bit_size == 28


def test_union(info: DebugInfo) -> None:
    payload = info.type("union payload")
    assert isinstance(payload, StructType)
    assert payload.union
    assert payload.size == 4
    by_name = {m.name: m for m in payload.members}
    assert by_name["as_u32"].offset == 0
    assert by_name["as_u32"].bit_size is None
    assert by_name["as_bytes"].offset == 0
    array = info.resolve(by_name["as_bytes"].type)
    assert isinstance(array, ArrayType)
    assert array.counts == (4,)


def test_self_referencing_struct(info: DebugInfo) -> None:
    list_node = info.type("struct list_node")
    assert isinstance(list_node, StructType)
    assert list_node.size == 8
    by_name = {m.name: m for m in list_node.members}
    next_ptr = info.resolve(by_name["next"].type)
    assert isinstance(next_ptr, PointerType)
    assert next_ptr.target is not None
    pointee = info.resolve(next_ptr.target)
    assert isinstance(pointee, StructType)
    assert pointee.name == "list_node"


def test_two_dimensional_array(info: DebugInfo) -> None:
    matrix = info.type("matrix_t")
    assert isinstance(matrix, Typedef)
    assert matrix.target is not None
    array = info.resolve(matrix.target)
    assert isinstance(array, ArrayType)
    assert array.counts == (3, 4)


def test_enum(info: DebugInfo) -> None:
    color = info.type("enum color")
    assert isinstance(color, EnumType)
    assert color.name == "color"
    assert color.size == 1
    assert dict(color.values) == {"color_red": 0, "color_green": 1, "color_blue": 2}


def test_typedef_chain_strips_to_enum(info: DebugInfo) -> None:
    hue = info.type("hue_t")
    assert isinstance(hue, Typedef)
    assert hue.target is not None
    shade = info.resolve(hue.target)
    assert isinstance(shade, Typedef)
    assert shade.target is not None
    color_t = info.resolve(shade.target)
    assert isinstance(color_t, Typedef)
    assert color_t.target is not None
    color = info.resolve(color_t.target)
    assert isinstance(color, EnumType)
    assert info.strip(info.types["hue_t"]) == color


def test_const_volatile_pointer(info: DebugInfo) -> None:
    reg_ptr = info.type("reg_ptr_t")
    assert isinstance(reg_ptr, Typedef)
    assert reg_ptr.target is not None
    const = info.resolve(reg_ptr.target)
    assert isinstance(const, Qualified)
    assert const.qualifier == Qualifier.CONST
    assert const.target is not None
    pointer = info.resolve(const.target)
    assert isinstance(pointer, PointerType)
    assert pointer.target is not None
    volatile = info.resolve(pointer.target)
    assert isinstance(volatile, Qualified)
    assert volatile.qualifier == Qualifier.VOLATILE
    assert volatile.target is not None
    base = info.resolve(volatile.target)
    assert isinstance(base, Typedef)
    assert base.name == "uint32_t"


def test_tag_and_ordinary_namespaces_are_separate(info: DebugInfo) -> None:
    assert "point" not in info.types
    assert "struct point" in info.types
    assert "color" not in info.types
    assert "enum color" in info.types
    assert "hue_t" in info.types


def test_nested_anonymous_union_in_struct(info: DebugInfo) -> None:
    wrapped = info.type("struct wrapped")
    assert isinstance(wrapped, StructType)
    assert wrapped.size == 8
    by_name = {m.name: m for m in wrapped.members}
    assert by_name["tag"].offset == 0
    anon = by_name[""]
    assert anon.offset == 4
    union = info.resolve(anon.type)
    assert isinstance(union, StructType)
    assert union.union
    assert union.name is None
    member_names = {m.name for m in union.members}
    assert member_names == {"as_u32", "as_bytes"}


def test_flexible_array_member(info: DebugInfo) -> None:
    flex = info.type("struct flex_array")
    assert isinstance(flex, StructType)
    by_name = {m.name: m for m in flex.members}
    array = info.resolve(by_name["data"].type)
    assert isinstance(array, ArrayType)
    assert array.counts == (None,)


def test_out_of_line_abstract_origin_instance(info: DebugInfo) -> None:
    triple_value = info.functions["triple_value"]
    assert triple_value.address == 0x08000008
    assert len(triple_value.params) == 1
    assert triple_value.params[0].name == "x"
    assert triple_value.decl is not None
    assert triple_value.decl.line == 113


def test_cross_cu_declaration_after_definition_keeps_address(info: DebugInfo) -> None:
    types_touch = info.functions["types_touch"]
    assert types_touch.address == 0x08000064


def test_ambiguous_duplicate_static_variable_is_omitted(info: DebugInfo) -> None:
    assert "shared_counter" not in info.variables


def test_variadic_function(info: DebugInfo) -> None:
    fn = info.functions["sum_variadic"]
    assert fn.variadic
    assert len(fn.params) == 1
    assert fn.params[0].name == "count"


def test_function_taking_and_returning_struct(info: DebugInfo) -> None:
    make_point = info.functions["make_point"]
    assert make_point.returns is not None
    returns = info.resolve(make_point.returns)
    assert isinstance(returns, StructType)
    assert returns.name == "point"

    sum_point = info.functions["sum_point"]
    assert len(sum_point.params) == 1
    param_type = info.resolve(sum_point.params[0].type)
    assert isinstance(param_type, StructType)
    assert param_type.name == "point"


def test_variable_addresses(info: DebugInfo) -> None:
    assert info.variables["node_a"].address == 0x2000005C
    assert info.variables["node_b"].address == 0x20000064
    assert info.variables["global_matrix"].address == 0x2000002C
    assert info.variables["global_flags"].address == 0x20000028
    assert info.variables["global_payload"].address == 0x20000024
    assert info.variables["global_color"].address == 0x20000020
    assert info.variables["status_reg"].address == 0x08000114
    assert info.variables["global_wrapped"].address == 0x20000014


def test_function_addresses_thumb_bit_cleared(info: DebugInfo) -> None:
    assert info.functions["make_point"].address == 0x0800000E
    assert info.functions["sum_point"].address == 0x08000014
    assert info.functions["sum_variadic"].address == 0x08000020
    assert info.functions["types_touch"].address == 0x08000064
    assert info.functions["call_triple"].address == 0x0800004C
    assert info.functions["flex_len"].address == 0x08000048


def test_function_decls(info: DebugInfo) -> None:
    make_point_decl = info.functions["make_point"].decl
    assert make_point_decl is not None
    assert make_point_decl.file == SOURCE_FILE
    assert make_point_decl.line == 43

    sum_point_decl = info.functions["sum_point"].decl
    assert sum_point_decl is not None
    assert sum_point_decl.line == 51

    sum_variadic_decl = info.functions["sum_variadic"].decl
    assert sum_variadic_decl is not None
    assert sum_variadic_decl.line == 56


@pytest.mark.skipif(
    shutil.which("arm-none-eabi-addr2line") is None, reason="arm-none-eabi-addr2line not on PATH"
)
def test_decl_file_matches_addr2line(info: DebugInfo) -> None:
    result = subprocess.run(
        ["arm-none-eabi-addr2line", "-e", str(ELF_PATH), "0x8000014"],
        capture_output=True,
        text=True,
        check=True,
    )
    toolchain_file, _, _ = result.stdout.strip().partition(":")
    decl = info.functions["sum_point"].decl
    assert decl is not None
    assert posixpath.normpath(toolchain_file) == decl.file


def test_missing_file_raises() -> None:
    with pytest.raises(DebugInfoError):
        load_debug_info(REPO_ROOT / "tests" / "firmware" / "image" / "prebuilt" / "no-such.elf")


@pytest.mark.parametrize("name", ["repro_sibling_strp_sup.elf", "repro_sibling_data4.elf"])
def test_unsupported_sibling_form_raises(name: str) -> None:
    with pytest.raises(DebugInfoError):
        load_debug_info(REPO_ROOT / "tests" / "python" / "image" / "data" / name)


@pytest.mark.skipif(
    shutil.which("arm-none-eabi-objcopy") is None, reason="no arm-none-eabi-objcopy"
)
def test_stripped_elf_raises(tmp_path: Path) -> None:
    stripped = tmp_path / "stripped.elf"
    subprocess.run(
        ["arm-none-eabi-objcopy", "--strip-debug", str(ELF_PATH), str(stripped)], check=True
    )
    with pytest.raises(DebugInfoError):
        load_debug_info(stripped)


def test_second_load_returns_same_object() -> None:
    assert load_debug_info(ELF_PATH) is load_debug_info(ELF_PATH)


def test_repeated_loads_resolve_the_path_once(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    copy = tmp_path / "copy.elf"
    shutil.copyfile(ELF_PATH, copy)
    resolve = Path.resolve
    calls: list[Path] = []

    def counting(self: Path, strict: bool = False) -> Path:
        calls.append(self)
        return resolve(self, strict)

    monkeypatch.setattr(Path, "resolve", counting)
    first = load_debug_info(copy)
    for _ in range(100):
        assert load_debug_info(copy) is first
    assert len(calls) == 1


def test_a_parse_runs_with_the_collector_off_and_restores_it(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parse = dwarf._load  # pyright: ignore[reportPrivateUsage]
    seen: list[bool] = []

    def watched(path: Path) -> DebugInfo:
        seen.append(gc.isenabled())
        return parse(path)

    monkeypatch.setattr(dwarf, "_load", watched)
    for index, enabled in enumerate((True, False)):
        copy = tmp_path / f"collector-{index}.elf"
        shutil.copyfile(ELF_PATH, copy)
        if enabled:
            gc.enable()
        else:
            gc.disable()
        try:
            load_debug_info(copy)
            assert gc.isenabled() is enabled
        finally:
            gc.enable()
    assert seen == [False, False]


@pytest.mark.timeout(70)
def test_overlapping_parses_restore_the_collector_once_both_end(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    parse = dwarf._load  # pyright: ignore[reportPrivateUsage]
    inside = threading.Barrier(2)
    first_done = threading.Event()
    during_second: list[bool] = []

    def watched(path: Path) -> DebugInfo:
        inside.wait(timeout=30)
        if path.name == "first.elf":
            info = parse(path)
            first_done.set()
            return info
        first_done.wait(timeout=30)
        during_second.append(gc.isenabled())
        return parse(path)

    monkeypatch.setattr(dwarf, "_load", watched)
    paths = [tmp_path / "first.elf", tmp_path / "second.elf"]
    for path in paths:
        shutil.copyfile(ELF_PATH, path)
    assert gc.isenabled()
    threads = [threading.Thread(target=load_debug_info, args=(path,)) for path in paths]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=60)
    assert during_second == [False]
    assert gc.isenabled()


def test_a_retargeted_symlink_loads_its_new_target(tmp_path: Path) -> None:
    a = tmp_path / "a.elf"
    b = tmp_path / "b.elf"
    shutil.copyfile(ELF_PATH, a)
    shutil.copyfile(ELF_PATH, b)
    link = tmp_path / "link.elf"
    link.symlink_to(a)
    assert load_debug_info(link) is load_debug_info(a)
    link.unlink()
    link.symlink_to(b)
    assert load_debug_info(link) is load_debug_info(b)
    assert load_debug_info(b) is not load_debug_info(a)


def test_strip_raises_on_cycle() -> None:
    cyclic = DebugInfo(
        by_id={1: Typedef(name="a", target=2), 2: Typedef(name="b", target=1)},
        types={},
        variables={},
        functions={},
    )
    with pytest.raises(DebugInfoError):
        cyclic.strip(1)


def test_encoding_keeps_signedness_on_char(info: DebugInfo) -> None:
    signed_char = info.type("signed char")
    assert isinstance(signed_char, BaseType)
    assert signed_char.encoding == Encoding.SIGNED
    unsigned_char = info.type("unsigned char")
    assert isinstance(unsigned_char, BaseType)
    assert unsigned_char.encoding == Encoding.UNSIGNED


class _FakeAttr:
    def __init__(self, form: str, value: object) -> None:
        self.form = form
        self.value = value


class _FakeDIE:
    def __init__(self, attributes: dict[str, _FakeAttr]) -> None:
        self.attributes = attributes


class _FakeRangeLists:
    def __init__(self, entries: Sequence[object]) -> None:
        self._entries = entries

    def get_range_list_at_offset(self, offset: int, cu: object = None) -> Sequence[object]:
        return self._entries


class _FakeDwarfInfo:
    def __init__(self, entries: Sequence[object]) -> None:
        self._range_lists = _FakeRangeLists(entries)

    def range_lists(self) -> _FakeRangeLists:
        return self._range_lists


class _FakeCU:
    def __init__(self, entries: Sequence[object], low_pc: int | None = None) -> None:
        self.dwarfinfo = _FakeDwarfInfo(entries)
        top_attrs: dict[str, _FakeAttr] = {}
        if low_pc is not None:
            top_attrs["DW_AT_low_pc"] = _FakeAttr("DW_FORM_addr", low_pc)
        self._top = _FakeDIE(top_attrs)

    def get_top_DIE(self) -> _FakeDIE:
        return self._top


def _ranges_die() -> _FakeDIE:
    return _FakeDIE({"DW_AT_ranges": _FakeAttr("DW_FORM_sec_offset", 0)})


def _call(cu: _FakeCU, die: _FakeDIE) -> int | None:
    return address_from_ranges(cast(CompileUnit, cu), cast(DIE, die))


def test_address_from_ranges_no_attribute_is_none() -> None:
    cu = _FakeCU([])
    assert _call(cu, _FakeDIE({})) is None


def test_address_from_ranges_absolute_entries() -> None:
    entries = [
        RangeEntry(0, 0, 0x2000, 0x2010, True),
        RangeEntry(0, 0, 0x1000, 0x1010, True),
    ]
    cu = _FakeCU(entries)
    assert _call(cu, _ranges_die()) == 0x1000


def test_address_from_ranges_base_relative_entries() -> None:
    entries = [
        BaseAddressEntry(0, 0x8000),
        RangeEntry(0, 0, 0x20, 0x30, False),
        BaseAddressEntry(0, 0x100),
        RangeEntry(0, 0, 0x5, 0x9, False),
    ]
    cu = _FakeCU(entries)
    assert _call(cu, _ranges_die()) == 0x105


def test_address_from_ranges_uses_cu_low_pc_as_initial_base() -> None:
    entries = [RangeEntry(0, 0, 0x10, 0x20, False)]
    cu = _FakeCU(entries, low_pc=0x5000)
    assert _call(cu, _ranges_die()) == 0x5010

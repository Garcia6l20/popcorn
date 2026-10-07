"""DWARF-typed variables over guest memory."""

from __future__ import annotations

import re

import popcorn
import pytest
from popcorn import Var
from popcorn.image.dwarf import BaseType, DebugInfo, Encoding, StructType


def test_struct_array_member_equals_a_raw_read(views: popcorn.Machine) -> None:
    table = views.var("table")
    target = table.entries[3].target
    assert target.value == 40
    assert target.value == views.mem.u16[target.address]
    assert target.address == table.address + 4 + 3 * table.entries[0].size + 2
    assert len(table.entries) == 5


def test_scalars_enums_floats_bitfields_and_pointers(views: popcorn.Machine) -> None:
    table = views.var("table")
    assert table.count.value == 5
    assert table.entries[1].kind.value == 7
    assert table.entries[1].kind.enum_name == "ROUTE_CAN"
    assert table.entries[2].gain.value == 2.5
    assert table.flags.value == 5
    assert table.mode.value == 17
    assert table.delta.value == -3
    assert table.offset.value == -300
    assert table.scale.value == 2.25
    current = table.current.deref()
    assert current.address == table.entries[3].address
    assert current.source.value == 4


def test_char_is_unsigned_and_an_enum_with_negatives_is_signed(views: popcorn.Machine) -> None:
    extras = views.var("extras")
    assert extras.letter.value == 0xE9
    assert extras.low.value == -2
    assert extras.low.enum_name == "LEVEL_LOW"
    extras.low.value = 2
    assert extras.low.enum_name == "LEVEL_HIGH"


def test_a_utf_char_type_reads_unsigned(views: popcorn.Machine) -> None:
    info = DebugInfo(
        by_id={1: BaseType("char16_t", 2, Encoding.CHAR)},
        types={"char16_t": 1},
        variables={},
        functions={},
    )
    views.mem.u16[0x2000_8000] = 0xFFFE
    assert Var(views.mem, info, 1, 0x2000_8000, "wide").value == 0xFFFE


def test_bitfield_writes_check_their_range(views: popcorn.Machine) -> None:
    table = views.var("table")
    with pytest.raises(ValueError, match=re.escape("3-bit field table.flags, 0 to 7")):
        table.flags.value = 8
    with pytest.raises(ValueError, match=re.escape("table.flags")):
        table.flags.value = -1
    with pytest.raises(ValueError, match=re.escape("4-bit field table.delta, -8 to 7")):
        table.delta.value = 8
    table.delta.value = -8
    assert table.delta.value == -8
    assert table.flags.value == 5


def test_writes_go_through_to_memory(views: popcorn.Machine) -> None:
    table = views.var("table")
    table.mode.value = 3
    assert table.mode.value == 3
    assert table.flags.value == 5
    table.entries[0].gain.value = -1.0
    assert views.mem.f32[table.entries[0].gain.address] == -1.0
    with pytest.raises(ValueError):
        table.count.value = -1


def test_multi_dimensional_arrays_index_by_row(views: popcorn.Machine) -> None:
    matrix = views.var("matrix")
    assert matrix[1][2].value == 6
    assert len(matrix) == 2
    assert len(matrix[0]) == 3
    with pytest.raises(IndexError):
        _ = matrix[2]


def test_types_and_unknown_names(views: popcorn.Machine) -> None:
    assert isinstance(views.types["struct route"], StructType)
    assert "route_table" in views.types
    with pytest.raises(popcorn.SymbolError) as caught:
        views.var("tabel")
    assert "table" in caught.value.close_matches
    with pytest.raises(AttributeError, match="no member"):
        _ = views.var("table").nope
    with pytest.raises(TypeError, match="not a scalar"):
        _ = views.var("table").value


def test_member_reaches_members_hidden_by_var_properties(views: popcorn.Machine) -> None:
    shadowed = views.var("shadowed")
    assert shadowed.member("size").value == 7
    assert shadowed.member("address").value == 0x1234
    struct = views.types["struct shadowed"]
    assert isinstance(struct, StructType)
    assert shadowed.size == struct.size
    assert shadowed.address != 0x1234
    kind = shadowed.kind.member("value")
    assert kind.value == 7
    assert kind.enum_name == "ROUTE_CAN"
    assert kind.name == "shadowed.kind.value"
    with pytest.raises(AttributeError, match="members: size, address, kind"):
        shadowed.member("value")

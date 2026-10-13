"""DWARF-typed variables over guest memory, and over the ELF files through `m.image`."""

from __future__ import annotations

import asyncio
import os
import re
import struct
from collections.abc import Iterator
from pathlib import Path

import popcorn
import pytest
from popcorn import Var
from popcorn._image_view import _ElfBytes as ElfBytes  # pyright: ignore[reportPrivateUsage]
from popcorn.image.dwarf import ArrayType, BaseType, DebugInfo, Encoding, StructType

from .conftest import VIEWS_ELF, run_bounded


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


HEX = Path(__file__).parents[2] / "native" / "image" / "data" / "image.hex"
GAINS = struct.pack("<4H", 100, 250, 400, 1000)


def leaves(var: Var) -> Iterator[Var]:
    if var.fields:
        for name in var.fields:
            yield from leaves(var.member(name))
    elif isinstance(var.type, ArrayType):
        for index in range(len(var)):
            yield from leaves(var[index])
    else:
        yield var


def test_image_view_reads_a_flash_table_while_running(views: popcorn.Machine) -> None:
    async def scenario() -> int | float | bool:
        async with views.running():
            with pytest.raises(popcorn.MachineRunning):
                _ = views.var("gains")[2].value
            return views.image.var("gains")[2].value

    assert run_bounded(scenario()) == 400


@pytest.mark.parametrize("name", ["table", "banner", "extras", "matrix", "gains", "shadowed"])
def test_image_view_equals_the_machine_after_load(views: popcorn.Machine, name: str) -> None:
    initial = list(leaves(views.image.var(name)))
    loaded = list(leaves(views.var(name)))
    assert [leaf.name for leaf in initial] == [leaf.name for leaf in loaded]
    assert [leaf.value for leaf in initial] == [leaf.value for leaf in loaded]
    assert len(initial) > 1


def test_image_view_follows_pointers_inside_the_file(views: popcorn.Machine) -> None:
    current = views.image.var("table").current.deref()
    assert current.address == views.var("table").entries[3].address
    assert current.source.value == 4


def test_image_view_reads_initial_values_not_what_the_firmware_wrote(
    views: popcorn.Machine,
) -> None:
    asyncio.run(views.run(budget=1_000))
    assert views.var("counter").value != 0
    assert views.image.var("counter").value == 0
    views.var("table").count.value = 9
    assert views.image.var("table").count.value == 5


def test_image_view_writes_raise_and_leave_the_machine_alone(views: popcorn.Machine) -> None:
    views.var("counter").value = 3
    with pytest.raises(TypeError, match="read-only"):
        views.image.var("counter").value = 1
    with pytest.raises(TypeError, match="read-only"):
        views.image.var("table").mode.value = 3
    assert views.var("counter").value == 3
    assert views.var("table").mode.value == 17


def test_image_view_of_a_hex_image_has_no_variables(m33: popcorn.PartDescription) -> None:
    m = popcorn.Machine(m33, image=popcorn.Image.hex(HEX))
    with pytest.raises(popcorn.SymbolError):
        m.image.var("gains")
    with pytest.raises(popcorn.SymbolError):
        m.var("gains")


def test_image_view_reads_the_file_now(m33: popcorn.PartDescription, tmp_path: Path) -> None:
    elf = tmp_path / "views.elf"
    elf.write_bytes(VIEWS_ELF.read_bytes())
    m = popcorn.Machine(m33, image=popcorn.Image.elf(elf))
    assert m.image.var("gains")[2].value == 400
    data = elf.read_bytes()
    assert data.count(GAINS) == 1
    elf.write_bytes(data.replace(GAINS, struct.pack("<4H", 100, 250, 401, 1000)))
    stat = elf.stat()
    os.utime(elf, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
    assert m.image.var("gains")[2].value == 401


def test_image_view_raises_outside_loadable_segments(views: popcorn.Machine) -> None:
    elf = ElfBytes(VIEWS_ELF)
    end = views.var("counter").address + views.var("counter").size
    assert elf.read(end - 4, 4) == bytes(4)
    with pytest.raises(popcorn.ImageError, match="no loadable segment") as caught:
        elf.read(end + 0x1_0000, 4)
    assert caught.value.address == end + 0x1_0000
    with pytest.raises(popcorn.ImageError, match="cross the edge") as caught:
        elf.read(end - 2, 4)
    assert caught.value.segment.isdigit()

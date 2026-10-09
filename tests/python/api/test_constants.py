"""Enumerators and enum types of ELF files read from DWARF with no machine, `Constants`."""

from __future__ import annotations

import re
from pathlib import Path

import popcorn
import pytest
from popcorn import _core
from popcorn.image import load_debug_info
from popcorn.image.dwarf import EnumType

from .conftest import ROOT, VIEWS_ELF, run_bounded

pytest_plugins = ["pytester"]

CONSTS_ELF = ROOT / "tests" / "firmware" / "api" / "prebuilt" / "consts.elf"


@pytest.fixture
def constants() -> popcorn.Constants:
    return popcorn.Elf(CONSTS_ELF).constants


def test_an_enumerator_reads_without_a_machine(constants: popcorn.Constants) -> None:
    assert constants["CHANNEL_MAX"] == 2
    assert constants.enumerator("LINK_SPI") == 7


def test_a_typedef_of_an_anonymous_enum_gives_its_members_in_order(
    constants: popcorn.Constants,
) -> None:
    channel = constants.enum("channel_t")
    assert [(member.name, member.value) for member in channel] == [
        ("CHANNEL_A", 0),
        ("CHANNEL_B", 1),
        ("CHANNEL_MAX", 2),
    ]
    assert channel["CHANNEL_B"] == 1
    assert channel.__module__ == "popcorn.firmware"


def test_tag_and_typedef_reach_one_class_with_aliases(constants: popcorn.Constants) -> None:
    link = constants.enum("enum link")
    assert link["LINK_DEFAULT"] is constants.enum("link")["LINK_UART"]
    assert constants.enum("link_t") is link
    assert popcorn.Elf(CONSTS_ELF).constants.enum("link") is link


def test_negative_and_high_enumerators_keep_their_sign(constants: popcorn.Constants) -> None:
    assert constants["LINK_NONE"] == -1
    assert constants["LINK_HIGH"] == 0x8000_0000


def test_an_enumerator_two_units_disagree_on_needs_a_unit(constants: popcorn.Constants) -> None:
    with pytest.raises(popcorn.SymbolError) as raised:
        _ = constants["MODE_FAST"]
    assert {Path(unit).name for unit in raised.value.close_matches} == {
        "consts_a.c",
        "consts_b.c",
    }
    assert "= 1" in str(raised.value) and "= 2" in str(raised.value)
    assert constants.enumerator("MODE_FAST", unit="consts_b.c") == 2
    assert constants.enumerator("MODE_FAST", unit="api/consts_a.c") == 1
    with pytest.raises(popcorn.SymbolError, match="matches 0 units"):
        constants.enumerator("MODE_FAST", unit="nope.c")


def test_an_enum_type_two_units_disagree_on_needs_a_unit(constants: popcorn.Constants) -> None:
    with pytest.raises(popcorn.SymbolError, match="different enumerators") as raised:
        constants.enum("shape_t")
    assert {Path(unit).name for unit in raised.value.close_matches} == {
        "consts_a.c",
        "consts_b.c",
    }
    shape = constants.enum("shape_t", unit="consts_b.c")
    assert [member.name for member in shape] == ["SHAPE_SQUARE", "SHAPE_OVAL"]


def test_a_type_that_is_no_enum_raises_type_error(constants: popcorn.Constants) -> None:
    with pytest.raises(TypeError, match="not an enum"):
        constants.enum("uint32_t")
    with pytest.raises(TypeError, match="BaseType, not an enum"):
        constants.enum("unsigned int")
    views = popcorn.Elf(VIEWS_ELF).constants
    for name in ("route", "struct route"):
        with pytest.raises(TypeError, match="StructType, not an enum"):
            views.enum(name)


def test_a_unit_that_only_declares_an_enum_is_ignored(constants: popcorn.Constants) -> None:
    opaque = constants.enum("opaque")
    assert [(member.name, member.value) for member in opaque] == [("OPAQUE_A", 4)]
    with pytest.raises(TypeError, match="declaration with no enumerators"):
        constants.enum("opaque", unit="consts_b.c")
    with pytest.raises(TypeError, match="declaration with no enumerators"):
        constants.enum("hidden")


def test_a_unit_of_a_header_enum_gives_the_same_class(constants: popcorn.Constants) -> None:
    assert constants.enum("link", unit="consts_b.c") is constants.enum("link")
    assert constants.enum("link_t", unit="consts_a.c") is constants.enum("enum link")


def test_an_enum_nothing_uses_is_absent(constants: popcorn.Constants) -> None:
    with pytest.raises(popcorn.SymbolError, match="UNUSED_A"):
        _ = constants["UNUSED_A"]
    with pytest.raises(popcorn.SymbolError, match=r"close matches: .*CHANNEL_MAX"):
        _ = constants["CHANNEL_MAXX"]


def test_the_last_file_hides_the_earlier_ones() -> None:
    project = popcorn.Project(
        part="STM32U575QGIx", images=[popcorn.Elf(VIEWS_ELF), popcorn.Elf(CONSTS_ELF)]
    )
    assert project.constants["ROUTE_CAN"] == 9
    assert project.constants["ROUTE_UART"] == 3


def test_an_absent_file_raises_missing_input_at_the_lookup(tmp_path: Path) -> None:
    absent = tmp_path / "absent.elf"
    constants = popcorn.Elf(absent).constants
    with pytest.raises(popcorn.MissingInput) as raised:
        _ = constants["X"]
    assert raised.value.path == absent
    assert raised.value.key == "path"


def test_a_truncated_file_raises_image_error_naming_it(tmp_path: Path) -> None:
    truncated = tmp_path / "consts.elf"
    data = CONSTS_ELF.read_bytes()
    truncated.write_bytes(data[: len(data) // 2])
    with pytest.raises(popcorn.ImageError, match=re.escape(str(truncated))):
        _ = popcorn.Elf(truncated).constants["CHANNEL_MAX"]


def test_image_constants_read_while_running(views: popcorn.Machine) -> None:
    async def scenario() -> int:
        async with views.running():
            return views.image.constants["ROUTE_CAN"]

    assert run_bounded(scenario()) == 7


def test_every_enum_die_lies_in_one_unit() -> None:
    info = load_debug_info(CONSTS_ELF)
    assert [Path(unit.name).name for unit in info.units] == ["consts_a.c", "consts_b.c"]
    for type_id, debug_type in info.by_id.items():
        if isinstance(debug_type, EnumType):
            holding = [unit for unit in info.units if unit.start <= type_id < unit.end]
            assert holding == [info.unit(type_id)]


def test_an_enum_parametrizes_at_collection_without_a_machine(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    def refused(*args: object, **kwargs: object) -> popcorn.Machine:
        raise AssertionError("a machine was built at collection")

    monkeypatch.setattr(popcorn.Project, "machine", refused)
    pytester.makepyfile(f"""
        from pathlib import Path

        import popcorn
        import pytest

        ELF = popcorn.Elf(Path({str(CONSTS_ELF)!r}))
        PROJECT = popcorn.Project(part="STM32U575QGIx", images=[ELF])


        @pytest.mark.parametrize("channel", list(PROJECT.constants.enum("channel_t")))
        def test_channel(channel):
            assert 0 <= channel <= 2
        """)
    result = pytester.runpytest_inprocess("-q")
    result.assert_outcomes(passed=3)


def test_the_native_macro_reader_lists_both_units() -> None:
    index = _core.read_macros(str(CONSTS_ELF))
    assert [Path(unit).name for unit in index.units()] == ["consts_a.c", "consts_b.c"]
    assert [entry.text for entry in index.lookup("CONSTS_MODE")] == ["1", "2"]

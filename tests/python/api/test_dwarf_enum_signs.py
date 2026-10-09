"""Sign extension of DWARF enumerators decoded from an unsigned data form."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import cast

import pytest
from elftools.dwarf.die import DIE
from popcorn.image.dwarf import _build_enum_type  # pyright: ignore[reportPrivateUsage]

DW_ATE_SIGNED = 0x05
DW_ATE_SIGNED_CHAR = 0x06
DW_ATE_UNSIGNED = 0x08


@dataclass
class Attribute:
    """What `_build_enum_type` reads of an attribute."""

    form: str
    value: object


@dataclass(frozen=True)
class FakeUnit:
    """The compile unit size `_safe_children` bounds a walk with."""

    size: int = 1024


@dataclass
class FakeDie:
    """A DIE as `_build_enum_type` walks it."""

    tag: str
    offset: int
    attributes: dict[str, Attribute] = field(default_factory=dict[str, Attribute])
    children: list[FakeDie] = field(default_factory=list["FakeDie"])
    target: FakeDie | None = None
    cu: FakeUnit = FakeUnit()

    def iter_children(self) -> list[FakeDie]:
        return self.children

    def get_DIE_from_attribute(self, name: str) -> FakeDie:
        assert name == "DW_AT_type" and self.target is not None
        return self.target


def base(encoding: int, size: int) -> FakeDie:
    return FakeDie(
        "DW_TAG_base_type",
        1,
        {
            "DW_AT_encoding": Attribute("DW_FORM_data1", encoding),
            "DW_AT_byte_size": Attribute("DW_FORM_data1", size),
        },
    )


def enum(size: int, form: str, value: int, underlying: FakeDie | None) -> int:
    enumerator = FakeDie(
        "DW_TAG_enumerator",
        3,
        {
            "DW_AT_name": Attribute("DW_FORM_string", b"E"),
            "DW_AT_const_value": Attribute(form, value),
        },
    )
    attributes = {
        "DW_AT_name": Attribute("DW_FORM_string", b"e"),
        "DW_AT_byte_size": Attribute("DW_FORM_data1", size),
    }
    if underlying is not None:
        attributes["DW_AT_type"] = Attribute("DW_FORM_ref4", underlying.offset)
    die = FakeDie("DW_TAG_enumeration_type", 2, attributes, [enumerator], underlying)
    return _build_enum_type(cast(DIE, die)).values["E"]


@pytest.mark.parametrize(
    ("size", "form", "value", "underlying", "expected"),
    [
        (1, "DW_FORM_data1", 0xFF, base(DW_ATE_SIGNED_CHAR, 1), -1),
        (4, "DW_FORM_data4", 0x8000_0000, base(DW_ATE_SIGNED, 4), -(2**31)),
        (4, "DW_FORM_data4", 0x7FFF_FFFF, base(DW_ATE_SIGNED, 4), 0x7FFF_FFFF),
        (4, "DW_FORM_data4", 0x8000_0000, base(DW_ATE_UNSIGNED, 4), 0x8000_0000),
        (4, "DW_FORM_data4", 0x8000_0000, None, 0x8000_0000),
        (4, "DW_FORM_sdata", -1, base(DW_ATE_SIGNED, 4), -1),
    ],
    ids=["signed-char", "int-min", "int-max", "unsigned", "no-type", "sdata"],
)
def test_a_data_form_enumerator_is_sign_extended_only_under_a_signed_type(
    size: int, form: str, value: int, underlying: FakeDie | None, expected: int
) -> None:
    assert enum(size, form, value, underlying) == expected

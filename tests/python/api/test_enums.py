"""Python enums against the native enumerations they mirror."""

from __future__ import annotations

import enum

import popcorn
import pytest
from popcorn import _core

PAIRS: list[tuple[type[enum.Enum], type[enum.Enum]]] = [
    (popcorn.StopReason, _core.StopReason),
    (popcorn.FaultKind, _core.FaultKind),
    (popcorn.ResetKind, _core.ResetKind),
    (popcorn.FloatAbi, _core.FloatAbi),
    (popcorn.ValueKind, _core.ValueKind),
]


@pytest.mark.parametrize(("python", "native"), PAIRS, ids=lambda t: t.__name__)
def test_python_enum_matches_native_names_and_values(
    python: type[enum.Enum], native: type[enum.Enum]
) -> None:
    assert {m.name: m.value for m in python} == {m.name.upper(): m.value for m in native}

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
    (popcorn.WatchKind, _core.WatchKind),
    (popcorn.WatchAction, _core.WatchAction),
    (popcorn.Initiator, _core.Initiator),
    (popcorn.board.PinMode, _core.PinMode),
]


@pytest.mark.parametrize(("python", "native"), PAIRS, ids=lambda t: t.__name__)
def test_python_enum_matches_native_names_and_values(
    python: type[enum.Enum], native: type[enum.Enum]
) -> None:
    assert {m.name: m.value for m in python} == {m.name.upper(): m.value for m in native}


def test_i2c_error_matches_the_native_errors_but_none() -> None:
    native = {m.name.upper(): m.value for m in _core.I2CError if m.value != 0}
    assert {m.name: m.value for m in popcorn.board.I2CError} == native
    assert _core.I2CError(0).name == "none"


def test_uart_error_matches_the_native_errors_but_none() -> None:
    native = {m.name.upper(): m.value for m in _core.UartError if m.value != 0}
    assert {m.name: m.value for m in popcorn.board.UartError} == native
    assert _core.UartError(0).name == "none"

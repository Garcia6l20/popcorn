"""Annotations of a device subclass are read without evaluating them: no future import here."""

import pytest
from popcorn.board import I2CTarget, UartDevice


def test_an_annotation_naming_a_class_defined_later_defines_fine() -> None:
    class Chip(UartDevice):
        later: Missing
        mine: Chip
        other: Chip | None = None

    class Missing:
        pass

    assert Chip.__name__ == "Chip"


def test_a_self_reference_in_an_i2c_target_defines_fine() -> None:
    class Node(I2CTarget):
        next: Node | None

    assert Node.__name__ == "Node"


def test_a_reserved_annotated_name_is_refused_beside_a_forward_reference() -> None:
    source = "class Chip(UartDevice):\n    later: Missing\n    _reset: Missing\n"
    with pytest.raises(TypeError, match="'_reset'"):
        exec(source, {"UartDevice": UartDevice})

"""Device bases reserve the private names popcorn uses: a subclass defining one is refused."""

from __future__ import annotations

import ast
import inspect
import textwrap
from typing import ClassVar

import popcorn
import pytest
from popcorn.board import I2CController, I2CTarget, SpiController, SpiTarget, UartDevice

BASES: list[type] = [UartDevice, I2CTarget, SpiTarget, I2CController, SpiController]


def internal_names(base: type) -> list[str]:
    tree = ast.parse(textwrap.dedent(inspect.getsource(base)))
    names: set[str] = set()
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Attribute)
            and isinstance(node.ctx, ast.Store)
            and isinstance(node.value, ast.Name)
            and node.value.id == "self"
            and not node.attr.startswith("__")
        ):
            names.add(node.attr)
    cls = tree.body[0]
    assert isinstance(cls, ast.ClassDef)
    for node in cls.body:
        if isinstance(node, ast.FunctionDef):
            names.add(node.name)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
        elif isinstance(node, ast.Assign):
            names.update(t.id for t in node.targets if isinstance(t, ast.Name))
    return sorted(n for n in names if n.startswith("_") and not n.startswith("__"))


def noop(self: object) -> None:
    return None


def define(base: type, **body: object) -> type:
    return type("Mine", (base,), body)


@pytest.mark.parametrize("base", BASES, ids=lambda base: base.__name__)
def test_a_subclass_defining_any_private_name_of_its_base_is_refused(base: type) -> None:
    names = internal_names(base)
    assert names
    for name in names:
        with pytest.raises(TypeError, match=rf"Mine.*'{name}'.*{base.__name__}.*rename it"):
            define(base, **{name: noop})


@pytest.mark.parametrize("base", BASES, ids=lambda base: base.__name__)
def test_a_private_name_of_the_base_is_refused_as_an_annotation_or_a_slot(base: type) -> None:
    name = internal_names(base)[0]
    with pytest.raises(TypeError, match=name):
        define(base, __annotations__={name: int})
    with pytest.raises(TypeError, match=name):
        define(base, __slots__=(name,))


@pytest.mark.parametrize("base", BASES, ids=lambda base: base.__name__)
def test_a_subclass_of_a_subclass_is_refused_too(base: type) -> None:
    mine = define(base)
    name = internal_names(base)[0]
    with pytest.raises(TypeError, match=name):
        type("Deeper", (mine,), {name: noop})


def test_a_subclass_reset_with_another_signature_is_refused() -> None:
    def reset(self: object, packet: object) -> None:
        pass

    with pytest.raises(TypeError, match=r"'_reset'.*UartDevice.*rename it"):
        define(UartDevice, _reset=reset)


def test_a_subclass_helper_with_another_private_name_still_works() -> None:
    class Chip(UartDevice):
        _table: ClassVar[dict[int, int]] = {1: 2}

        def __init__(self) -> None:
            super().__init__()
            self.seen = self._look(1)

        def _look(self, key: int) -> int:
            return self._table[key]

    assert Chip().seen == 2


@pytest.mark.parametrize("base", BASES[1:], ids=lambda base: base.__name__)
def test_a_bus_subclass_helper_with_another_private_name_is_accepted(base: type) -> None:
    mine = define(base, _helper=noop, _data=3)
    assert mine.__name__ == "Mine"


def test_every_taken_name_is_reported() -> None:
    with pytest.raises(TypeError, match=r"'_reset', '_save'.*private names of UartDevice.*them"):
        define(UartDevice, _reset=noop, _save=noop)


class SetsReset(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        vars(self)["_reset"] = 3


class SetsRead(I2CTarget):
    def __init__(self) -> None:
        super().__init__(0x50)
        vars(self)["_read"] = []


class SetsFrame(SpiController):
    def __init__(self) -> None:
        super().__init__(sck_hz=1_000_000)
        vars(self)["_frame"] = 1


class SetsQueue(I2CController):
    def __init__(self) -> None:
        super().__init__()
        vars(self)["_queue"] = 1


@pytest.mark.parametrize(
    ("device", "peripheral", "name", "base"),
    [
        (SetsReset, "USART1", "_reset", "UartDevice"),
        (SetsRead, "I2C1", "_read", "I2CTarget"),
        (SetsFrame, "SPI1", "_frame", "SpiController"),
        (SetsQueue, "I2C1", "_queue", "I2CController"),
    ],
)
def test_an_instance_attribute_named_like_a_reserved_name_is_refused_when_attached(
    u575: popcorn.PartDescription, device: type, peripheral: str, name: str, base: str
) -> None:
    with pytest.raises(TypeError, match=rf"{device.__name__} sets '{name}'.*{base}.*rename it"):
        popcorn.Machine(u575, devices={peripheral: [device()]})


def test_a_chip_select_attribute_set_on_the_instance_is_accepted(
    u575: popcorn.PartDescription,
) -> None:
    class Chip(SpiTarget):
        def __init__(self) -> None:
            super().__init__("PA4")
            self._cs_port = "GPIOB"

    popcorn.Machine(u575, devices={"SPI1": [Chip()]})

"""The build refuses a target reaching both popcorn core flavours, whatever path reaches them."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass, field

from popcorn_build.core_links import Linked, both_flavours, reached


@dataclass(eq=False)
class Node:
    name: str
    target_type: str | None
    public: list[Node] = field(default_factory=lambda: [])
    private: list[Node] = field(default_factory=lambda: [])

    def linked_targets(self, *, private: bool = True) -> Sequence[Linked]:
        return [*self.public, *(self.private if private else [])]


def cores() -> tuple[Node, Node]:
    return Node("popcorn_core", "shared_library"), Node("popcorn_core_static", "static_library")


def test_a_program_on_one_flavour_passes() -> None:
    shared, static = cores()
    support = Node("support", "static_library", public=[shared])
    test = Node("test", "program", private=[support, shared])
    tool = Node("tool", "program", private=[static])
    assert both_flavours([shared, static, support, test, tool], shared, static) == []


def test_a_program_on_both_flavours_is_refused() -> None:
    shared, static = cores()
    support = Node("support", "static_library", public=[shared])
    test = Node("test", "program", private=[support, static])
    assert both_flavours([shared, static, support, test], shared, static) == ["test"]


def test_a_shared_library_linking_a_flavour_privately_still_counts() -> None:
    shared, static = cores()
    area = Node("popcorn_usb", "shared_library", private=[shared])
    test = Node("test", "program", private=[area, static])
    assert both_flavours([shared, static, area, test], shared, static) == ["test"]
    assert shared in reached(test)


def test_a_cycle_ends_the_walk() -> None:
    shared, static = cores()
    first = Node("first", "static_library")
    second = Node("second", "static_library", public=[first, shared])
    first.public.append(second)
    test = Node("test", "program", private=[first])
    assert set(map(id, reached(test))) == {id(first), id(second), id(shared)}
    assert both_flavours([test], shared, static) == []

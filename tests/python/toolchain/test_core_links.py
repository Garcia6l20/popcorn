"""The build refuses a target reaching both popcorn library flavours, whatever path reaches them."""

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
    assert both_flavours([shared, static, support, test, tool], [shared], [static]) == []


def test_a_program_on_both_flavours_is_refused() -> None:
    shared, static = cores()
    support = Node("support", "static_library", public=[shared])
    test = Node("test", "program", private=[support, static])
    assert both_flavours([shared, static, support, test], [shared], [static]) == ["test"]


def test_a_shared_library_linking_a_flavour_privately_still_counts() -> None:
    shared, static = cores()
    area = Node("popcorn_usb", "shared_library", private=[shared])
    test = Node("test", "program", private=[area, static])
    assert both_flavours([shared, static, area, test], [shared], [static]) == ["test"]
    assert shared in reached(test)


def test_a_cycle_ends_the_walk() -> None:
    shared, static = cores()
    first = Node("first", "static_library")
    second = Node("second", "static_library", public=[first, shared])
    first.public.append(second)
    test = Node("test", "program", private=[first])
    assert set(map(id, reached(test))) == {id(first), id(second), id(shared)}
    assert both_flavours([test], [shared], [static]) == []


def areas() -> tuple[list[Node], list[Node]]:
    shared_core, static_core = cores()
    shared = [shared_core, Node("popcorn_armv8m", "shared_library", public=[shared_core])]
    static = [static_core, Node("popcorn_armv8m_static", "static_library", public=[static_core])]
    shared.append(Node("popcorn_stm32", "shared_library", public=[shared_core]))
    static.append(Node("popcorn_stm32_static", "static_library", public=[static_core]))
    return shared, static


def test_a_static_test_whose_support_links_a_shared_area_is_refused() -> None:
    shared, static = areas()
    support = Node("popcorn-test-support-static", "static_library", public=[shared[1], static[0]])
    test = Node("test-core-run-loop", "program", private=[support, static[0]])
    assert both_flavours([*shared, *static, support, test], shared, static) == [
        "test-core-run-loop"
    ]


def test_two_areas_of_different_flavours_are_refused_without_a_core_named() -> None:
    shared, static = areas()
    test = Node("test", "program", private=[static[2], shared[1]])
    assert both_flavours([test], shared[1:], static[1:]) == ["test"]


def test_every_area_of_one_flavour_passes() -> None:
    shared, static = areas()
    on_shared = Node("on-shared", "program", private=shared[1:])
    on_static = Node("on-static", "program", private=static[1:])
    targets = [*shared, *static, on_shared, on_static]
    assert both_flavours(targets, shared, static) == []

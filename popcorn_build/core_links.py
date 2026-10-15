"""Which popcorn library flavour each target reaches, free of the build's side effects."""

from __future__ import annotations

from collections.abc import Collection, Iterable, Sequence
from typing import Protocol


class Linked(Protocol):
    """What the flavour check reads of a pcons target."""

    @property
    def name(self) -> str: ...

    @property
    def target_type(self) -> str | None: ...

    def linked_targets(self, *, private: bool = True) -> Sequence[Linked]: ...


def reached(target: Linked) -> list[Linked]:
    """Every target @p target links, directly or through any dependency, private links included.

    Private links of shared libraries count too: every shared library's dependencies are loaded
    into the same process.
    """
    found: list[Linked] = []
    seen: set[int] = set()
    pending = list(target.linked_targets(private=True))
    while pending:
        dependency = pending.pop()
        if id(dependency) in seen:
            continue
        seen.add(id(dependency))
        found.append(dependency)
        pending.extend(dependency.linked_targets(private=True))
    return found


def both_flavours(
    targets: Iterable[Linked], shared: Collection[Linked], static: Collection[Linked]
) -> list[str]:
    """Names of the programs and shared libraries among @p targets reaching both flavours.

    @p shared and @p static hold every popcorn library of each flavour. Reaching one of each,
    directly or through any library, puts two copies of an area in one process.
    """
    shared_ids = {id(item) for item in shared}
    static_ids = {id(item) for item in static}
    refused: list[str] = []
    for target in targets:
        if target.target_type not in ("program", "shared_library"):
            continue
        linked = {id(item) for item in reached(target)}
        if linked & shared_ids and linked & static_ids:
            refused.append(target.name)
    return refused

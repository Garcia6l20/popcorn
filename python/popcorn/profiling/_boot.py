"""The timeline of milestones, from the first block at each address."""

from __future__ import annotations

from dataclasses import dataclass

from popcorn import _core


@dataclass(frozen=True, slots=True)
class Milestone:
    """The first time the code reached one address."""

    name: str
    """Symbol name as given, the address in hex for an address."""
    address: int
    """Address, a translated block's start: a function start or a branch target."""
    time_ps: int | None
    """Profile time of the first block starting at `address`, None when none ran. For a
    profile opened at the reset with no reset or restore inside, the machine time."""


def timeline(
    result: _core.ProfileResult, milestones: tuple[tuple[str, int], ...]
) -> tuple[Milestone, ...]:
    """The milestones, in the order given."""
    first = {block.pc: block.first_ps for block in result.code}
    return tuple(Milestone(name, address, first.get(address)) for name, address in milestones)

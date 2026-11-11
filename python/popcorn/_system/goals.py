from __future__ import annotations

import asyncio
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, cast

from popcorn import _core
from popcorn._enums import StopReason
from popcorn._stop import Stop, stop_of

if TYPE_CHECKING:
    pass


@dataclass(frozen=True)
class Pause:
    reason: _core.PauseReason
    member: int | None
    at: int
    stop: Stop
    error: BaseException | None
    windows: int
    slices: int

    @staticmethod
    def of(raw: object) -> Pause:
        reason, member, at, info, error, windows, slices = cast(
            tuple[
                _core.PauseReason,
                int | None,
                int,
                _core.StopInfo,
                BaseException | None,
                int,
                int,
            ],
            raw,
        )
        return Pause(reason, member, at, stop_of(info), error, windows, slices)


@dataclass(eq=False)
class Wait:
    until: int | None
    for_ps: int | None
    deadline_ps: int | None
    gate: _core.Predicate | None
    stop_when: Callable[[], bool] | None
    future: asyncio.Future[Stop]
    until_ps: int | None = None
    from_executed: int | None = None


@dataclass(eq=False)
class Goal:
    future: _core.HostFuture
    task: asyncio.Task[object] | None
    releases: bool = False
    withdrawn: bool = False
    waits: tuple[Wait, ...] = ()

    def done(self) -> bool:
        return self.future.done()


def reached_waits(waits: Sequence[Wait], stop: Stop, at: int, held: int) -> list[Wait]:
    """The waits a run goal serving `waits` reached with `stop` at system time `at`, `held`
    the mask of waits whose predicate stopped it: none for a member stop."""
    if stop.reason is StopReason.TIME and stop.detail == "goal cap":
        return []
    reached: list[Wait] = []
    for bit, wait in enumerate(waits):
        if stop.reason is StopReason.USER:
            own = bool(held >> bit & 1)
        elif stop.reason is StopReason.TIME:
            own = wait.until_ps is not None and wait.until_ps <= at
        elif stop.reason is StopReason.ADDRESS:
            own = wait.until == stop.pc
        else:
            own = False
        if own:
            reached.append(wait)
    return reached


def held_of(raw: object) -> tuple[Stop, int, int]:
    info, at, held = cast(tuple[_core.StopInfo, int, int], raw)
    return stop_of(info), at, held


def reached_of(raw: object) -> tuple[Stop, int]:
    info, at = cast(tuple[_core.StopInfo, int], raw)
    return stop_of(info), at

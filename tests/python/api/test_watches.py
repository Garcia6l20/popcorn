"""Watches on guest memory."""

from __future__ import annotations

import asyncio

import popcorn
import pytest


def test_a_watch_without_handler_stops_on_the_write_naming_the_writer(
    hooked: popcorn.Machine,
) -> None:
    watch = hooked.watch("shared")
    result = asyncio.run(hooked.call("write_three", budget=100_000))
    assert result.stop.reason is popcorn.StopReason.WATCHPOINT
    assert result.stop.detail
    assert len(watch.hits) == 1
    hit = watch.hits[0]
    assert hooked.locate(hit.pc).symbol == "writer"
    assert (hit.address, hit.size, hit.value, hit.write) == (hooked.sym("shared"), 4, 1, True)
    assert hit.initiator is popcorn.Initiator.CPU


def test_a_resuming_handler_counts_every_write(hooked: popcorn.Machine) -> None:
    values: list[int] = []

    def count(hit: popcorn.WatchHit) -> popcorn.WatchAction:
        values.append(hit.value)
        return popcorn.WatchAction.RESUME

    with hooked.watch("shared", handler=count) as watch:
        result = asyncio.run(hooked.call("write_three", budget=100_000))
        assert result.stop.reason is popcorn.StopReason.RETURNED
    assert values == [1, 2, 3]
    assert not watch.active


def test_reads_are_not_writes(hooked: popcorn.Machine) -> None:
    watch = hooked.watch("shared", kind=popcorn.WatchKind.READ, handler=lambda hit: None)
    asyncio.run(hooked.call("write_three", budget=100_000))
    assert watch.hits == []


def test_a_watch_on_nothing_is_a_config_error(hooked: popcorn.Machine) -> None:
    with pytest.raises(popcorn.ConfigError):
        hooked.watch(0x3000_0000)
    with pytest.raises(popcorn.ConfigError):
        hooked.watch(0x2000_0000, size=0)

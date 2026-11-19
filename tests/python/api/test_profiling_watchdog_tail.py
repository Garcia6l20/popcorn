"""The IWDG example refreshing past the profile write log cap, and the reload tracker."""

from __future__ import annotations

import asyncio
import inspect
from contextlib import AbstractContextManager

import popcorn
import pytest
from popcorn import _core
from popcorn._observe import ProfileRecording
from popcorn.profiling._watchdog import WatchdogRegisters, watchdog_registers
from popcorn.units import ms, s

from .test_profiling import booted
from .test_profiling_extras import IWDG

BURST = 5000
INIT_KR_WRITES = 3
KEY_VALUES = (0xAAAA, 0xCCCC)

type Held = tuple[int, int, int]


def registers_of(m: popcorn.Machine) -> WatchdogRegisters:
    dog = watchdog_registers(m.description, lambda at: int.from_bytes(m.mem.read(at, 4), "little"))
    assert dog is not None
    return dog


def tracked(
    m: popcorn.Machine, dog: WatchdogRegisters, limit: int
) -> AbstractContextManager[ProfileRecording]:
    return m._profile(  # pyright: ignore[reportPrivateUsage]
        logged_registers=dog.logged,
        write_log_limit=limit,
        write_clock_domain="lsi",
        reload_key_ids=dog.kr,
        reload_change_ids=(*dog.pr, *dog.rlr),
        reload_values=KEY_VALUES,
    )


def run_burst(m: popcorn.Machine) -> None:
    m.var("refresh_burst").value = BURST
    for _ in range(200):
        asyncio.run(m.run(for_=ms(5)))
        if m.var("refresh_burst").value == 0:
            return
    raise AssertionError("the burst did not finish")


def test_a_refresh_burst_past_the_log_cap_drops_the_rest_and_counts_them() -> None:
    """`HAL_IWDG_Init` ends with three KR stores: the 0xCCCC start, the 0x5555 unlock and, with
    the window disabled, the 0xAAAA reload."""
    m = booted(IWDG)
    profile_fn = popcorn.Machine._profile  # pyright: ignore[reportPrivateUsage]
    limit = inspect.signature(profile_fn).parameters["write_log_limit"].default
    dog = registers_of(m)
    refreshes = [0]

    def count(call: popcorn.Call) -> popcorn.Return:
        refreshes[0] += 1
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", count)
    with m._profile(  # pyright: ignore[reportPrivateUsage]
        logged_registers=dog.logged, write_log_limit=limit, write_clock_domain="lsi"
    ) as recording:
        run_burst(m)
    log = next(log for log in recording.result.write_logs if log.id in dog.kr)
    assert len(log.writes) == limit == 4096
    assert refreshes[0] >= BURST
    assert log.dropped >= BURST - limit
    assert len(log.writes) + log.dropped == refreshes[0] + INIT_KR_WRITES


def brute_force(result: _core.ProfileResult, dog: WatchdogRegisters) -> tuple[
    list[int],
    dict[tuple[int, int], Held],
    dict[int, tuple[int, int, int]],
    list[tuple[_core.EpochKind, int, int, int, int, int]],
]:
    tagged = sorted(
        ((write.record, log.id, write) for log in result.write_logs for write in log.writes),
        key=lambda item: item[0],
    )
    counts = [0, 0]
    longest: dict[tuple[int, int], Held] = {}
    tails: dict[int, tuple[int, int, int]] = {}
    previous: tuple[_core.ProfileWrite, int, int] | None = None
    none = 0xFFFF_FFFF
    epochs = [(_core.EpochKind.start, 0, 0, 0, 0, none)]
    last_key = none
    epoch = 0
    segment = 0
    for _, register, write in tagged:
        if write.segment != segment:
            epoch += 1
            segment = write.segment
            previous = None
            last_key = none
            epochs.append((_core.EpochKind.cut, segment, 0, 0, 0, none))
        if register in (*dog.pr, *dog.rlr):
            epoch += 1
            epochs.append(
                (_core.EpochKind.change, segment, register, write.at, write.value, last_key)
            )
            continue
        last_key = write.value & 0xFFFF
        if (write.value & 0xFFFF) not in KEY_VALUES:
            continue
        kind = KEY_VALUES.index(write.value & 0xFFFF)
        counts[kind] += 1
        if previous is not None:
            first, first_kind, first_epoch = previous
            gap = write.ps - first.ps
            if gap > longest.get((first_epoch, first_kind), (-1, 0, 0))[0]:
                longest[first_epoch, first_kind] = (gap, first.at, write.at)
        previous = (write, kind, epoch)
        tails[segment] = (write.at, kind, epoch)
    return counts, longest, tails, epochs


def test_the_tracker_matches_a_brute_force_pass_over_an_uncapped_log() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    with tracked(m, dog, 1_000_000) as recording:
        asyncio.run(m.run(for_=ms(250)))
        m.var("reconfigure_prescaler").value = 1
        m.var("reconfigure_reload").value = 800
        asyncio.run(m.run(for_=ms(250)))
        assert m.var("reconfigure_reload").value == 0
        m.var("refresh_enabled").value = 0
        stop = asyncio.run(m.run(for_=s(1)))
        assert stop.reason is popcorn.StopReason.RESET_REQUEST
        m.reset(popcorn.ResetKind.WATCHDOG)
        asyncio.run(m.run(for_=ms(350)))
    result = recording.result
    (track,) = result.reload_tracks
    counts, longest, tails, epochs = brute_force(result, dog)
    assert track.counts == counts
    assert counts[0] > 5 and counts[1] == 2
    assert track.epochs_dropped == 0
    assert [
        (e.kind, e.segment, e.change_id, e.change.at, e.change.value, e.key_before)
        for e in track.epochs
    ] == epochs
    assert [epoch.kind for epoch in track.epochs].count(_core.EpochKind.cut) == 1
    assert [e.key_before for e in track.epochs if e.kind == _core.EpochKind.change][:2] == [
        0x5555
    ] * 2
    seen = {
        (index, kind): (gap.ps, gap.start.at, gap.end.at)
        for index, epoch in enumerate(track.epochs)
        for kind, gap in enumerate(epoch.longest)
        if gap is not None
    }
    assert seen == longest
    assert {tail.segment: (tail.last.at, tail.kind, tail.epoch) for tail in track.tails} == tails
    key = next(log for log in result.write_logs if log.id in dog.kr)
    assert track.key_writes == len(key.writes) + key.dropped


def test_the_tracker_counts_a_burst_past_the_default_log_cap_exactly() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    refreshes = [0]

    def count(call: popcorn.Call) -> popcorn.Return:
        refreshes[0] += 1
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", count)
    with tracked(m, dog, 4096) as recording:
        run_burst(m)
    (track,) = recording.result.reload_tracks
    assert track.counts == [refreshes[0] + 1, 1]
    assert refreshes[0] >= BURST
    assert track.epochs_dropped == 0
    (tail,) = track.tails
    assert tail.kind == 0


def test_a_restore_inside_a_profile_cuts_the_tracker() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    saved = m.snapshot()
    with tracked(m, dog, 4096) as recording:
        asyncio.run(m.run(for_=ms(250)))
        m.restore(saved)
        asyncio.run(m.run(for_=ms(250)))
    (track,) = recording.result.reload_tracks
    kinds = [epoch.kind for epoch in track.epochs]
    assert kinds.count(_core.EpochKind.cut) == recording.result.cuts >= 1
    assert [tail.segment for tail in track.tails] == list(range(recording.result.cuts + 1))


def test_a_tracker_without_a_known_clock_domain_is_refused() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    with (
        pytest.raises(ValueError, match="clock domain"),
        m._profile(  # pyright: ignore[reportPrivateUsage]
            reload_key_ids=dog.kr, reload_change_ids=dog.pr, reload_values=KEY_VALUES
        ),
    ):
        pass

"""The IWDG example refreshing past the profile write log cap, and the reload tracker."""

# pyright: reportPrivateUsage=false

from __future__ import annotations

import dataclasses
import inspect
import json
import random
from collections.abc import Sequence
from contextlib import AbstractContextManager
from itertools import pairwise

import popcorn
import pytest
from popcorn import _core
from popcorn._observe import ProfileRecording
from popcorn.profiling import (
    Report,
    _watchdog,
    diff,
)
from popcorn.profiling._watchdog import (
    COUNTER_RESET,
    REFRESH_KEY,
    START_KEY,
    UNLOCK_KEY,
    Watchdog,
    WatchdogGap,
    WatchdogRegisters,
    _divider,
    _expiry,
    _Held,
    watchdog,
    watchdog_registers,
)
from popcorn.units import ms, s

from .test_profiling import booted
from .test_profiling_extras import (
    IWDG,
    KR,
    LSI_HZ,
    PR,
    REGISTERS,
    RLR,
    Event,
    advance,
    setup,
    synthetic,
)

BURST = 5000
INIT_KR_WRITES = 3
KEY_VALUES = (0xAAAA, 0xCCCC)
KEY_MASK = 0xFFFF

type Held = tuple[int, int, int]


def registers_of(m: popcorn.Machine) -> WatchdogRegisters:
    dog = watchdog_registers(m.description, lambda at: int.from_bytes(m.mem.read(at, 4), "little"))
    assert dog is not None
    return dog


def tracked(
    m: popcorn.Machine, dog: WatchdogRegisters, limit: int
) -> AbstractContextManager[ProfileRecording]:
    return m._profile(
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
        advance(m, ms(5))
        if m.var("refresh_burst").value == 0:
            return
    raise AssertionError("the burst did not finish")


def test_a_refresh_burst_past_the_log_cap_drops_the_rest_and_counts_them() -> None:
    """`HAL_IWDG_Init` ends with three KR stores: the 0xCCCC start, the 0x5555 unlock and, with
    the window disabled, the 0xAAAA reload."""
    m = booted(IWDG)
    profile_fn = popcorn.Machine._profile
    limit = inspect.signature(profile_fn).parameters["write_log_limit"].default
    dog = registers_of(m)
    refreshes = [0]

    def count(call: popcorn.Call) -> popcorn.Return:
        refreshes[0] += 1
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", count)
    with m._profile(
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
        advance(m, ms(250))
        m.var("reconfigure_prescaler").value = 1
        m.var("reconfigure_reload").value = 800
        advance(m, ms(250))
        assert m.var("reconfigure_reload").value == 0
        m.var("refresh_enabled").value = 0
        advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
        m.reset(popcorn.ResetKind.WATCHDOG)
        advance(m, ms(350))
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
        advance(m, ms(250))
        m.restore(saved)
        advance(m, ms(250))
    (track,) = recording.result.reload_tracks
    kinds = [epoch.kind for epoch in track.epochs]
    assert kinds.count(_core.EpochKind.cut) == recording.result.cuts >= 1
    assert [tail.segment for tail in track.tails] == list(range(recording.result.cuts + 1))


def test_a_tracker_without_a_known_clock_domain_is_refused() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    with (
        pytest.raises(ValueError, match="clock domain"),
        m._profile(reload_key_ids=dog.kr, reload_change_ids=dog.pr, reload_values=KEY_VALUES),
    ):
        pass


LSI_PS = 1_000_000_000_000 // LSI_HZ


def oracle_watchdog(result: _core.ProfileResult, registers: WatchdogRegisters) -> Watchdog:
    kinds = {
        **dict.fromkeys(registers.kr, "kr"),
        **dict.fromkeys(registers.pr, "pr"),
        **dict.fromkeys(registers.rlr, "rlr"),
    }
    writes = sorted(
        (
            (write.segment, write.record, kinds[log.id], write)
            for log in result.write_logs
            if log.id in kinds
            for write in log.writes
        ),
        key=lambda item: (item[0], item[1]),
    )
    rates: dict[int, list[_core.ProfileRate]] = {}
    for rate in result.domain_rates:
        rates.setdefault(rate.segment, []).append(rate)
    segment_end: dict[int, int] = {}
    total = 0
    for clock in result.clocks:
        total += clock.ps
        segment_end[clock.segment] = total
    held = _Held(registers.pr_start, registers.rlr_start, None)
    segment = 0
    reloads: list[tuple[int, int, int, int | None]] = []
    refreshes = starts = 0
    for at_segment, _, kind, write in writes:
        if at_segment != segment:
            held, segment = _Held(None, None, None), at_segment
        if kind != "kr":
            held.write(kind, write.value, registers)
            continue
        key = write.value & KEY_MASK
        held.locked = key != UNLOCK_KEY
        if key not in (REFRESH_KEY, START_KEY):
            continue
        refreshes += key == REFRESH_KEY
        starts += key == START_KEY
        count = COUNTER_RESET if key == START_KEY else held.rlr
        expiry = (
            None
            if held.pr is None or count is None
            else _expiry(
                rates.get(at_segment, []),
                write.ps,
                write.record,
                _divider(held.pr, registers.prescaler_top) * count,
            )
        )
        timeout = None if expiry is None else expiry - write.ps
        reloads.append((at_segment, write.ps, write.pc, timeout))
    margin: int | None = None
    closest: int | None = None
    gaps: list[WatchdogGap] = []
    for (seg_a, ps_a, pc_a, timeout), (seg_b, ps_b, pc_b, _) in pairwise(reloads):
        if seg_a != seg_b or timeout is None:
            continue
        left = timeout - (ps_b - ps_a)
        if margin is None or left < margin:
            margin, closest = left, pc_b
        if left <= 0:
            gaps.append(WatchdogGap(ps_a, ps_b - ps_a, timeout, pc_a))
    for index, (seg, ps, pc, timeout) in enumerate(reloads):
        last_of_segment = index + 1 == len(reloads) or reloads[index + 1][0] != seg
        if last_of_segment and timeout is not None:
            length = segment_end.get(seg, ps) - ps
            if length >= timeout:
                gaps.append(WatchdogGap(ps, length, timeout, pc))
    gaps.sort(key=lambda gap: gap.start_ps)
    last = reloads[-1][3] if reloads else None
    return Watchdog(refreshes, starts, last, margin, closest, tuple(gaps), 0, None)


def assert_matches_oracle(dog: Watchdog, oracle: Watchdog, *, margin_tolerance_ps: int = 0) -> None:
    """The analysis equals the pairwise oracle in every field but `key_writes` and
    `logged_until_ps`. A tolerance relaxes the margin and leaves out `closest_pc`."""
    assert (dog.refreshes, dog.starts, dog.timeout_ps) == (
        oracle.refreshes,
        oracle.starts,
        oracle.timeout_ps,
    )
    assert dog.gaps == oracle.gaps
    if dog.min_margin_ps is None or oracle.min_margin_ps is None:
        assert dog.min_margin_ps == oracle.min_margin_ps
    else:
        assert abs(dog.min_margin_ps - oracle.min_margin_ps) <= margin_tolerance_ps
    if margin_tolerance_ps == 0:
        assert dog.closest_pc == oracle.closest_pc


def real_run(
    scenario: str, limit: int = 1_000_000
) -> tuple[_core.ProfileResult, WatchdogRegisters]:
    m = booted(IWDG)
    dog = registers_of(m)
    with tracked(m, dog, limit) as recording:
        match scenario:
            case "steady":
                advance(m, ms(600))
            case "stall":
                advance(m, ms(250))
                m.var("refresh_enabled").value = 0
                advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
            case "reset":
                advance(m, ms(250))
                m.var("refresh_enabled").value = 0
                advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
                m.reset(popcorn.ResetKind.WATCHDOG)
                advance(m, ms(350))
            case "reconfigure":
                advance(m, ms(250))
                m.var("reconfigure_prescaler").value = 1
                m.var("reconfigure_reload").value = 800
                advance(m, ms(400))
                m.var("refresh_enabled").value = 0
                advance(m, s(1), popcorn.StopReason.RESET_REQUEST)
            case "burst":
                advance(m, ms(150))
                run_burst(m)
                advance(m, ms(250))
            case _:
                raise AssertionError(scenario)
    return recording.result, dog


@pytest.mark.parametrize("scenario", ["steady", "stall", "reset", "burst", "reconfigure"])
def test_the_analysis_equals_the_oracle_on_real_runs(scenario: str) -> None:
    result, dog = real_run(scenario)
    got = watchdog(result, dog)
    assert got.refreshes > 0
    assert_matches_oracle(got, oracle_watchdog(result, dog))
    assert got.logged_until_ps is None


def synthetic_cases() -> dict[str, tuple[list[Event], int]]:
    cut = ms(100).ps
    return {
        "start_counts_from_0xfff": (
            [
                ("r", 0, LSI_HZ, 0),
                *setup(0, 0, 100),
                ("w", KR, 0xCCCC, 10),
                ("w", KR, 0xAAAA, ms(20).ps),
            ],
            ms(30).ps,
        ),
        "pr_rlr_unknown_after_a_cut": (
            [
                ("r", 0, LSI_HZ, 0),
                *setup(0, 3, 500),
                ("w", KR, 0xAAAA, 10),
                ("cut", 0, 0, cut),
                ("r", 0, LSI_HZ, cut),
                ("w", KR, 0xAAAA, cut + ms(1).ps),
                *setup(cut + ms(4).ps, 3, 500),
                ("w", KR, 0xAAAA, cut + ms(5).ps),
            ],
            cut + ms(6).ps,
        ),
        "rate_change_between_reloads": (
            [
                ("r", 0, LSI_HZ, 0),
                *setup(0, 3, 500),
                ("w", KR, 0xAAAA, 0),
                ("r", 0, 250, ms(10).ps),
                ("w", KR, 0xAAAA, ms(100).ps),
            ],
            ms(200).ps,
        ),
        "reload_while_lsi_stopped": (
            [
                ("r", 0, 0, 0),
                ("w", KR, 0xCCCC, 0),
                ("r", 0, LSI_HZ, ms(1).ps),
                ("w", KR, 0xAAAA, ms(2).ps),
            ],
            ms(4).ps,
        ),
        "single_reload": (
            [("r", 0, LSI_HZ, 0), *setup(0, 0, 100), ("w", KR, 0xAAAA, 10)],
            ms(30).ps,
        ),
    }


@pytest.mark.parametrize("name", list(synthetic_cases()))
def test_synthetic_cases_equal_the_oracle(name: str) -> None:
    events, end = synthetic_cases()[name]
    result = synthetic(events, end)
    assert_matches_oracle(watchdog(result, REGISTERS), oracle_watchdog(result, REGISTERS))


def reloads(times_ms: list[float]) -> list[Event]:
    return [("w", KR, 0xAAAA, int(at * 1_000_000_000)) for at in times_ms]


def test_several_gaps_past_the_timeout_in_one_epoch_list_the_longest_only() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 500),
        *reloads([1, 101, 301, 311]),
    ]
    result = synthetic(events, ms(400).ps)
    got = watchdog(result, REGISTERS)
    oracle = oracle_watchdog(result, REGISTERS)
    assert [gap.length_ps for gap in oracle.gaps] == [ms(100).ps, ms(200).ps, ms(89).ps]
    assert [gap.length_ps for gap in got.gaps] == [ms(200).ps, ms(89).ps]
    assert got.min_margin_ps == oracle.min_margin_ps == 4 * 500 * LSI_PS - ms(200).ps


def test_a_rate_change_inside_a_gap_reports_the_longest_ticks_pair_with_its_exact_margin() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 500),
        *reloads([1, 41]),
        ("r", 0, 2 * LSI_HZ, ms(60).ps),
        *reloads([70]),
    ]
    result = synthetic(events, ms(200).ps)
    got = watchdog(result, REGISTERS)
    oracle = oracle_watchdog(result, REGISTERS)
    assert got.min_margin_ps == ms(60.75).ps - ms(40).ps
    assert got.closest_pc == 0x0800_0100 + 2 * 5
    assert oracle.min_margin_ps == ms(11.75).ps


def test_a_pair_with_no_timeout_stands_for_its_epoch_and_kind() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 10),
        *reloads([1.0, 1.5]),
        ("r", 0, 0, int(ms(2.5).ps)),
        *reloads([4.0]),
    ]
    result = synthetic(events, ms(5).ps)
    got = watchdog(result, REGISTERS)
    oracle = oracle_watchdog(result, REGISTERS)
    assert oracle.min_margin_ps == 40 * LSI_PS - ms(0.5).ps
    assert got.min_margin_ps is None


def test_a_hot_refresher_past_the_cap_keeps_its_margin_and_count() -> None:
    period = 1_000_000
    events: list[Event] = [("r", 0, LSI_HZ, 0), *setup(0, 0, 500)]
    events += [("w", KR, 0xAAAA, 10 + n * period) for n in range(50_000)]
    events += [("w", KR, 0xAAAA, 10 + 50_000 * period + 5_000_000)]
    events += [("w", KR, 0xAAAA, 10 + 50_000 * period + 5_000_000 + 2 * period)]
    result = synthetic(events, 10 + 50_002 * period + 10_000_000, keep_logs=False)
    dog = watchdog(result, REGISTERS)
    assert dog.refreshes == 50_002
    assert dog.min_margin_ps == 4 * 500 * LSI_PS - (period + 5_000_000)
    assert dog.closest_pc == 0x0800_0100 + 2 * 50_004


def test_each_gap_is_judged_with_the_timeout_of_its_own_epoch() -> None:
    first = 200
    events: list[Event] = [("r", 0, LSI_HZ, 0), *setup(0, 0, 500)]
    events += reloads([1 + 50 * n for n in range(first)])
    last = 1 + 50 * (first - 1)
    events += [("w", KR, 0x5555, ms(last + 10).ps), ("w", PR, 1, ms(last + 10).ps + 1)]
    events += reloads([last + 70, last + 170, last + 270])
    dog = watchdog(synthetic(events, ms(last + 300).ps, keep_logs=False), REGISTERS)
    short = 4 * 500 * LSI_PS
    assert dog.min_margin_ps == short - ms(70).ps
    assert [gap.timeout_ps for gap in dog.gaps] == [short]
    assert dog.timeout_ps == 2 * short


def test_a_stall_in_the_first_segment_is_one_gap_and_the_second_segment_is_clean() -> None:
    cut = ms(700).ps
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 500),
        *reloads([1, 51, 101]),
        ("cut", 0, 0, cut),
        ("r", 0, LSI_HZ, cut),
        *setup(cut + 1, 0, 500),
        ("w", KR, 0xAAAA, cut + ms(1).ps),
        ("w", KR, 0xAAAA, cut + ms(51).ps),
    ]
    dog = watchdog(synthetic(events, cut + ms(100).ps), REGISTERS)
    assert len(dog.gaps) == 1
    gap = dog.gaps[0]
    assert gap.start_ps == ms(101).ps
    assert gap.length_ps == cut - ms(101).ps


def test_a_dropped_change_write_stops_the_analysis_where_the_table_stopped() -> None:
    timeout = 4 * 500 * LSI_PS
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        ("w", KR, 0x5555, 0),
        ("w", PR, 0, 1),
        ("w", RLR, 500, 2),
        *reloads([1, 51, 101]),
        ("w", PR, 0, ms(110).ps),
        *reloads([111, 900, 910]),
    ]
    result = synthetic(events, ms(1000).ps, limit=3)
    track = result.reload_tracks[0]
    assert track.epochs_dropped == 1
    dog = watchdog(result, REGISTERS)
    assert dog.logged_until_ps == track.frozen_ps == ms(110).ps
    assert (dog.refreshes, dog.key_writes) == (6, 7)
    assert dog.min_margin_ps == timeout - ms(50).ps
    assert dog.gaps == ()
    assert dog.timeout_ps is None


def test_a_dropped_cut_keeps_the_tail_of_the_segment_it_ended() -> None:
    cut = ms(900).ps
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        ("w", KR, 0x5555, 0),
        ("w", PR, 0, 1),
        *reloads([1, 51]),
        ("cut", 0, 0, cut),
        ("r", 0, LSI_HZ, cut),
        ("w", KR, 0xAAAA, cut + 1),
    ]
    result = synthetic(events, cut + ms(10).ps, limit=2)
    dog = watchdog(result, REGISTERS)
    assert result.reload_tracks[0].epochs_dropped == 1
    assert dog.logged_until_ps == cut
    assert dog.refreshes == 3
    assert [(gap.start_ps, gap.length_ps) for gap in dog.gaps] == [(ms(51).ps, cut - ms(51).ps)]
    assert dog.timeout_ps == dog.gaps[0].timeout_ps


def test_json_of_schema_2_round_trips_the_new_fields_and_the_diff_counts_them() -> None:
    reports: list[Report] = []
    for span in (ms(250), ms(600)):
        m = booted(IWDG)
        with m.profile("watchdog") as profile:
            advance(m, span)
        reports.append(profile.report())
    short, long = reports
    assert short.watchdog is not None and long.watchdog is not None
    assert short.watchdog.key_writes >= short.watchdog.refreshes
    assert Report.from_json(short.to_json()) == short
    changes = diff(short, long)["watchdog"]["watchdog"]
    assert changes["key_writes"] == long.watchdog.key_writes - short.watchdog.key_writes > 0


def test_a_part_with_no_watchdog_has_the_empty_section() -> None:
    result = _core.ProfileResult()
    assert watchdog(result, None) == Watchdog(0, 0, None, None, None, (), 0, None)


PRESCALED_PS = 4 * LSI_PS
BOUND_RL = 800


def phased(
    held: _Held,
    kind: int,
    registers: WatchdogRegisters,
    rates: Sequence[_core.ProfileRate],
    write: _core.ProfileWrite,
) -> int:
    return (BOUND_RL + 1) * PRESCALED_PS + (PRESCALED_PS - write.ps % PRESCALED_PS)


def true_smallest(times: list[int]) -> int:
    return min(
        phased(_Held(0, BOUND_RL, None), 0, REGISTERS, [], write_at(a)) - (b - a)
        for a, b in pairwise(times)
    )


def test_the_margin_differs_from_the_true_smallest_by_less_than_one_prescaled_period(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(_watchdog, "_reload_timeout", phased)
    first = 0
    second = BOUND_RL * PRESCALED_PS - 1000
    third = second + (second - first) - 1000
    times = [first, second, third]
    events: list[Event] = [("r", 0, LSI_HZ, 0), *(("w", KR, 0xAAAA, at) for at in times)]
    got = watchdog(synthetic(events, third + 10, keep_logs=False), REGISTERS)
    assert got.min_margin_ps is not None
    assert PRESCALED_PS // 2 < got.min_margin_ps - true_smallest(times) <= PRESCALED_PS


def test_the_margin_bound_holds_over_random_streams(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(_watchdog, "_reload_timeout", phased)
    rng = random.Random(20261119)
    widest = 0
    for _ in range(200):
        times = [rng.randrange(PRESCALED_PS)]
        for _ in range(rng.randrange(20, 41)):
            times.append(
                times[-1]
                + BOUND_RL * PRESCALED_PS
                + rng.randrange(-2 * PRESCALED_PS, 3 * PRESCALED_PS)
            )
        events: list[Event] = [("r", 0, LSI_HZ, 0), *(("w", KR, 0xAAAA, at) for at in times)]
        got = watchdog(synthetic(events, times[-1] + 10, keep_logs=False), REGISTERS)
        assert got.min_margin_ps is not None
        difference = got.min_margin_ps - true_smallest(times)
        assert 0 <= difference <= PRESCALED_PS
        widest = max(widest, difference)
    assert widest > PRESCALED_PS // 4


def write_at(ps: int) -> _core.ProfileWrite:
    write = _core.ProfileWrite()
    write.ps = ps
    return write


def test_a_schema_1_text_is_refused() -> None:
    with pytest.raises(ValueError, match="schema 2"):
        Report.from_json(json.dumps({"schema": 1, "report": {}}))


def burst_run(stall: bool) -> tuple[Watchdog, list[int], int]:
    m = booted(IWDG)
    seen: list[int] = []

    def record(call: popcorn.Call) -> popcorn.Return:
        seen.append(m.time.ps)
        return call.proceed()

    m.intercept("HAL_IWDG_Refresh", record)
    with m.profile("watchdog") as profile:
        advance(m, ms(250))
        run_burst(m)
        if stall:
            m.var("refresh_enabled").value = 0
            advance(m, s(2), popcorn.StopReason.RESET_REQUEST)
        else:
            advance(m, ms(300))
    report = profile.report()
    assert report.watchdog is not None
    return report.watchdog, seen, report.elapsed_ps


def test_a_burst_past_the_cap_then_a_stall_reports_the_real_numbers() -> None:
    dog, seen, elapsed = burst_run(stall=True)
    assert dog.timeout_ps is not None
    assert dog.refreshes == len(seen) + 1
    assert dog.starts == 1
    assert dog.key_writes > dog.refreshes + dog.starts
    assert dog.logged_until_ps is None
    (gap,) = dog.gaps
    assert gap.length_ps == gap.timeout_ps == dog.timeout_ps
    assert gap.start_ps + gap.length_ps == elapsed
    longest = max(b - a for a, b in pairwise(seen))
    assert dog.min_margin_ps == dog.timeout_ps - longest


def test_a_burst_past_the_cap_with_no_stall_has_no_gap() -> None:
    dog, seen, _ = burst_run(stall=False)
    assert dog.refreshes == len(seen) + 1
    assert dog.gaps == ()
    assert dog.min_margin_ps is not None and dog.min_margin_ps > 0


def test_the_logged_registers_add_nothing_to_the_analysis() -> None:
    results: list[Watchdog] = []
    for logged in (False, True):
        m = booted(IWDG)
        dog = registers_of(m)
        with m._profile(
            logged_registers=dog.logged if logged else (),
            write_clock_domain="lsi",
            reload_key_ids=dog.kr,
            reload_change_ids=(*dog.pr, *dog.rlr),
            reload_values=KEY_VALUES,
        ) as recording:
            run_burst(m)
            advance(m, ms(300))
        results.append(watchdog(recording.result, dog))
    assert results[0] == results[1]


def test_machine_profile_requests_the_tracker_and_no_logged_register() -> None:
    m = booted(IWDG)
    with m.profile("watchdog") as profile:
        advance(m, ms(250))
    result = profile._recording.result
    assert result.write_logs == []
    (track,) = result.reload_tracks
    assert track.counts[0] > 1


def test_each_segment_is_judged_with_its_own_lsi_rate() -> None:
    cut = ms(100).ps
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        *setup(0, 0, 500),
        *reloads([1, 11]),
        ("cut", 0, 0, cut),
        ("r", 0, 2 * LSI_HZ, cut),
        *setup(cut + 1, 0, 500),
        ("w", KR, 0xAAAA, cut + ms(1).ps),
        ("w", KR, 0xAAAA, cut + ms(41).ps),
    ]
    result = synthetic(events, cut + ms(60).ps)
    got = watchdog(result, REGISTERS)
    oracle = oracle_watchdog(result, REGISTERS)
    assert_matches_oracle(got, oracle)
    fast = 4 * 500 * LSI_PS // 2
    assert got.min_margin_ps == fast - ms(40).ps < 0
    assert (got.gaps[-1].start_ps, got.gaps[-1].timeout_ps) == (cut + ms(1).ps, fast)


def test_the_capped_log_gives_the_old_analysis_a_false_tail_and_a_partial_count() -> None:
    m = booted(IWDG)
    dog = registers_of(m)
    with tracked(m, dog, 4096) as recording:
        run_burst(m)
        advance(m, ms(700))
    result = recording.result
    key = next(log for log in result.write_logs if log.id in dog.kr)
    assert len(key.writes) == 4096 and key.dropped > 0
    old = oracle_watchdog(result, dog)
    new = watchdog(result, dog)
    assert old.refreshes + old.starts + 1 == 4096
    assert new.refreshes > BURST
    assert [gap.start_ps for gap in old.gaps] == [key.writes[-1].ps]
    assert old.gaps[0].length_ps >= old.gaps[0].timeout_ps
    assert new.gaps == ()


def shape(track: _core.ProfileReloadTrack, ids: dict[int, int]) -> tuple[object, ...]:
    def gap(held: _core.ProfileReloadGap | None) -> tuple[int, ...] | None:
        if held is None:
            return None
        return (held.start.ps, held.end.ps, held.ps, held.start.pc, held.end.pc)

    return (
        track.counts,
        track.key_writes,
        [
            (
                e.kind,
                e.segment,
                e.ps,
                ids.get(e.change_id, 0),
                e.change.value,
                e.change.pc,
                e.key_before,
                [gap(held) for held in e.longest],
            )
            for e in track.epochs
        ],
        [(t.segment, t.kind, t.epoch, t.last.ps, t.last.value, t.last.pc) for t in track.tails],
        track.epochs_dropped,
        track.frozen_ps if track.epochs_dropped else 0,
        track.frozen_segment if track.epochs_dropped else 0,
    )


def events_of(result: _core.ProfileResult, dog: WatchdogRegisters) -> tuple[list[Event], list[int]]:
    kind_of = {
        **dict.fromkeys(dog.kr, KR),
        **dict.fromkeys(dog.pr, PR),
        **dict.fromkeys(dog.rlr, RLR),
    }
    items: list[tuple[int, int, Event]] = [
        (write.record, 0, ("w", kind_of[log.id], write.value, write.ps))
        for log in result.write_logs
        for write in log.writes
    ]
    pcs = [
        write.pc
        for write in sorted(
            (write for log in result.write_logs for write in log.writes),
            key=lambda write: write.record,
        )
    ]
    items += [
        (epoch.record, 1, ("cut", 0, 0, epoch.ps))
        for epoch in result.reload_tracks[0].epochs
        if epoch.kind == _core.EpochKind.cut
    ]
    items += [(rate.record, 2, ("r", 0, rate.hz, rate.ps)) for rate in result.domain_rates]
    return [event for _, _, event in sorted(items, key=lambda item: item[:2])], pcs


@pytest.mark.parametrize(
    ("scenario", "limit"),
    [("steady", 1_000_000), ("reconfigure", 3), ("reset", 2), ("burst", 4)],
)
def test_the_python_mirror_equals_the_native_tracker(scenario: str, limit: int) -> None:
    full, dog = real_run(scenario)
    capped, _ = real_run(scenario, limit)
    ids = {**dict.fromkeys(dog.kr, KR), **dict.fromkeys(dog.pr, PR), **dict.fromkeys(dog.rlr, RLR)}
    events, pcs = events_of(full, dog)
    end = max(event[3] for event in events)
    mirror = synthetic(events, end, limit, keep_logs=False, pcs=pcs).reload_tracks[0]
    assert shape(capped.reload_tracks[0], ids) == shape(mirror, {KR: KR, PR: PR, RLR: RLR})


@pytest.mark.parametrize(("scenario", "limit"), [("reconfigure", 3), ("reset", 2)])
def test_a_real_run_past_the_epoch_cap_stops_the_analysis_where_the_table_stopped(
    scenario: str, limit: int
) -> None:
    full, dog = real_run(scenario)
    capped, _ = real_run(scenario, limit)
    (track,) = capped.reload_tracks
    got = watchdog(capped, dog)
    reference = watchdog(full, dog)
    assert track.epochs_dropped > 0
    assert got.logged_until_ps == track.frozen_ps
    assert (got.refreshes, got.starts, got.key_writes) == (
        reference.refreshes,
        reference.starts,
        reference.key_writes,
    )
    assert got.logged_until_ps is not None
    assert all(gap.start_ps < got.logged_until_ps for gap in got.gaps)
    assert reference.logged_until_ps is None


def test_json_of_a_report_with_logged_until_ps_set_round_trips() -> None:
    m = booted(IWDG)
    with m.profile("watchdog") as profile:
        advance(m, ms(250))
    report = profile.report()
    assert report.watchdog is not None
    cut = dataclasses.replace(
        report, watchdog=dataclasses.replace(report.watchdog, logged_until_ps=123)
    )
    back = Report.from_json(cut.to_json())
    assert back == cut
    assert back.watchdog is not None and back.watchdog.logged_until_ps == 123


def watchdog_text(dog: Watchdog) -> list[str]:
    from popcorn.profiling._report import _watchdog_lines

    return _watchdog_lines(dog)


def test_the_watchdog_line_prints_the_counts_and_the_key_writes() -> None:
    events: list[Event] = [("r", 0, LSI_HZ, 0), *setup(0, 0, 500), *reloads([1, 51, 101])]
    dog = watchdog(synthetic(events, ms(110).ps), REGISTERS)
    text = "\n".join(watchdog_text(dog))
    assert "refreshes 3, starts 0, timeout 62.500 ms," in text
    assert "key writes 4" in text
    assert "writes past the log" not in text
    assert "counts cover the whole profile" not in text
    assert "no reload for" not in text


def test_the_watchdog_text_notes_where_the_margin_stops_when_epochs_overflowed() -> None:
    events: list[Event] = [
        ("r", 0, LSI_HZ, 0),
        ("w", KR, 0x5555, 0),
        ("w", PR, 0, 1),
        *reloads([1, 51]),
        ("w", PR, 0, ms(60).ps),
        *reloads([61]),
    ]
    result = synthetic(events, ms(100).ps, limit=2)
    dog = watchdog(result, REGISTERS)
    lines = watchdog_text(dog)
    assert dog.logged_until_ps == ms(60).ps
    assert lines[-1] == (
        "  counts cover the whole profile, margin, closest pc, timeout and gaps stop at 60.000 ms"
    )


def test_the_report_text_of_a_burst_carries_the_exact_count_and_the_notes() -> None:
    m = booted(IWDG)
    with m.profile("watchdog") as profile:
        run_burst(m)
        advance(m, ms(200))
    report = profile.report()
    assert report.watchdog is not None
    text = report.text()
    assert f"refreshes {report.watchdog.refreshes:,}, starts 1," in text
    assert f"key writes {report.watchdog.key_writes:,}" in text
    assert "watchdog: refreshes, starts and key writes cover the whole profile" in text
    assert "exact for a constant LSI rate and stop at `logged_until_ps` when it is set" in text
    assert "a stretch holds one gap, its longest" in text
    assert "Intervals are ordered by LSI periods" in text

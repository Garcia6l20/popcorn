"""The independent watchdog margin, from the reload tracker of a profile."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass, replace
from fractions import Fraction

from popcorn import _core
from popcorn._part_description import PartDescription

PS_PER_SECOND = 1_000_000_000_000
REFRESH_KEY = 0xAAAA
START_KEY = 0xCCCC
UNLOCK_KEY = 0x5555
COUNTER_RESET = 0xFFF
"""Count a start reloads, the counter's reset value, RM0456 61.4.3."""
REFRESH_KIND = 0
START_KIND = 1
RELOAD_KEYS = (REFRESH_KEY, START_KEY)
"""Low 16 bits of a KR write that reloads, indexed by reload kind."""
NO_KEY = 0xFFFF_FFFF
"""`key_before` of a write with no earlier KR write in its segment."""


@dataclass(frozen=True, slots=True)
class WatchdogRegisters:
    """Flat register ids and values of the IWDG registers a profile logs, read at its entry."""

    kr: tuple[int, ...]
    """Flat ids of KR, one per IWDG alias, the Secure one included."""
    pr: tuple[int, ...]
    """Flat ids of PR."""
    rlr: tuple[int, ...]
    """Flat ids of RLR."""
    prescaler_top: int
    """Largest PR value with its own divider, 4 << PR."""
    pr_mask: int
    """Bits of the PR field."""
    rlr_mask: int
    """Bits of the RL field."""
    pr_start: int
    """PR at the profile start."""
    rlr_start: int
    """RLR at the profile start."""

    @property
    def logged(self) -> tuple[int, ...]:
        """Every id to log."""
        return (*self.kr, *self.pr, *self.rlr)


def watchdog_registers(
    description: PartDescription, read: Callable[[int], int]
) -> WatchdogRegisters | None:
    """The IWDG registers of `description`, with PR and RLR read through `read`, None when the
    part has no IWDG."""
    flat = 0
    ids: dict[str, list[int]] = {"KR": [], "PR": [], "RLR": []}
    found: list[tuple[int, _core.RegisterDesc, _core.RegisterDesc]] = []
    for peripheral in description.peripherals:
        for register in peripheral.registers:
            if peripheral.name.endswith("IWDG") and register.name in ids:
                ids[register.name].append(flat)
            flat += 1
        if peripheral.name == "IWDG":
            found.append((peripheral.base, peripheral.register("PR"), peripheral.register("RLR")))
    if not found or not ids["KR"]:
        return None
    base, pr, rlr = found[0]
    width = next(field.width for field in pr.fields if field.name == "PR")
    rl_width = next(field.width for field in rlr.fields if field.name == "RL")
    return WatchdogRegisters(
        tuple(ids["KR"]),
        tuple(ids["PR"]),
        tuple(ids["RLR"]),
        6 if width == 3 else 8,
        (1 << width) - 1,
        (1 << rl_width) - 1,
        read(base + pr.offset),
        read(base + rlr.offset),
    )


@dataclass(frozen=True, slots=True)
class WatchdogGap:
    """A span from a reload with no further reload before the timeout: the watchdog could have
    expired in it, at the earliest prescaler phase."""

    start_ps: int
    """Profile time of the reload."""
    length_ps: int
    """Picoseconds to the next reload, or to the end of its segment when none followed."""
    timeout_ps: int
    """Time from the reload to the expiry, see `Watchdog.timeout_ps`."""
    pc: int
    """Address of the reloading write."""


@dataclass(frozen=True, slots=True)
class Watchdog:
    """Independent watchdog reloads and the margin each left before the timeout."""

    refreshes: int
    """Refreshes, KR writes of 0xAAAA."""
    starts: int
    """Starts, KR writes of 0xCCCC."""
    timeout_ps: int | None
    """Time from the last reload to its earliest expiry: PR and RLR in force, RL + 1 periods of
    the prescaled LSI from the key write, 0xFFF + 1 for the start, each part at the LSI rate it
    ran at. The model resets a refresh RL + 1 to RL + 2 periods after the write, by the phase of
    the free-running prescaler, and a start exactly 0xFFF + 1 periods after it. None without a
    reload, with PR or RLR unknown, or with the LSI stopped from the reload on."""
    min_margin_ps: int | None
    """Smallest timeout less the time to the next reload, over the longest pair of consecutive
    reloads of each epoch and reload kind, the pair with the most LSI periods between its
    reloads. Each pair margin is exact in picoseconds. It is the smallest margin of the profile
    while the LSI rate is constant and every pair has a timeout. A rate change inside a timeout
    window can leave another pair with a smaller margin, and a longest pair with no timeout
    leaves its epoch and kind out. Negative when the watchdog could have expired between them,
    at the earliest prescaler phase: the model resets up to one prescaled period later, by the
    phase of the write against the free-running prescaler."""
    closest_pc: int | None
    """Address of the reload that ended the pair of `min_margin_ps`. Equal margins go to the
    first epoch, then the lowest reload kind."""
    gaps: tuple[WatchdogGap, ...]
    """Spans at or past the timeout with no reload, the span after the last reload of each
    segment included. Per epoch and reload kind only the longest span is listed."""
    key_writes: int
    """KR writes of any key, the whole profile."""
    logged_until_ps: int | None
    """Profile time up to which `min_margin_ps`, `closest_pc`, `timeout_ps` and `gaps` hold, None
    when they hold for the whole profile. `refreshes`, `starts` and `key_writes` always cover the
    whole profile."""


def _divider(pr: int, top: int) -> int:
    return 4 << min(pr, top)


def _expiry(
    rates: Sequence[_core.ProfileRate], start_ps: int, record: int, ticks: int
) -> int | None:
    """Profile time at which `ticks` LSI periods have passed from a write at `start_ps`, the
    `record`-th, with `rates` its segment's LSI rates. None when the LSI stops before."""
    hz = 0
    later: list[tuple[int, int]] = []
    for rate in rates:
        if rate.record < record:
            hz = rate.hz
        else:
            later.append((rate.ps, rate.hz))
    left = Fraction(ticks)
    at = start_ps
    for ps, rate in later:
        span = Fraction((ps - at) * hz, PS_PER_SECOND)
        if hz and left <= span:
            break
        left -= span
        at, hz = ps, rate
    if hz == 0:
        return None
    return at + int(left * PS_PER_SECOND // hz)


@dataclass(slots=True)
class _Held:
    pr: int | None
    rlr: int | None
    locked: bool | None
    """True after a key other than 0x5555, False after 0x5555, None when unknown."""

    def write(self, kind: str, value: int, registers: WatchdogRegisters) -> None:
        if self.locked:
            return
        unlocked = self.locked is False
        if kind == "pr":
            self.pr = value & registers.pr_mask if unlocked else None
        else:
            self.rlr = value & registers.rlr_mask if unlocked else None


def _reload_timeout(
    held: _Held,
    kind: int,
    registers: WatchdogRegisters,
    rates: Sequence[_core.ProfileRate],
    write: _core.ProfileWrite,
) -> int | None:
    """Picoseconds from the reload `write` of reload kind `kind` to the expiry under `held`, None
    when it is unknown."""
    count = COUNTER_RESET if kind == START_KIND else held.rlr
    expiry = (
        None
        if held.pr is None or count is None
        else _expiry(
            rates,
            write.ps,
            write.record,
            _divider(held.pr, registers.prescaler_top) * (count + 1),
        )
    )
    return None if expiry is None else expiry - write.ps


def _snapshots(track: _core.ProfileReloadTrack, registers: WatchdogRegisters) -> list[_Held]:
    """The PR and RLR in force in each epoch of `track`."""
    kinds = {
        **dict.fromkeys(registers.pr, "pr"),
        **dict.fromkeys(registers.rlr, "rlr"),
    }
    held = _Held(registers.pr_start, registers.rlr_start, None)
    snapshots: list[_Held] = []
    for epoch in track.epochs:
        match epoch.kind:
            case _core.EpochKind.start:
                held = _Held(registers.pr_start, registers.rlr_start, None)
            case _core.EpochKind.cut:
                held = _Held(None, None, None)
            case _core.EpochKind.change:
                held.locked = None if epoch.key_before == NO_KEY else epoch.key_before != UNLOCK_KEY
                held.write(kinds[epoch.change_id], epoch.change.value, registers)
        snapshots.append(replace(held))
    return snapshots


def watchdog(result: _core.ProfileResult, registers: WatchdogRegisters | None) -> Watchdog:
    """The watchdog section of a profile result, from its reload track. The empty section for a
    part with no IWDG and for a result with no reload track."""
    if registers is None or not result.reload_tracks:
        return Watchdog(0, 0, None, None, None, (), 0, None)
    track = result.reload_tracks[0]
    rates: dict[int, list[_core.ProfileRate]] = {}
    for rate in result.domain_rates:
        rates.setdefault(rate.segment, []).append(rate)
    segment_end: dict[int, int] = {}
    total = 0
    for clock in result.clocks:
        total += clock.ps
        segment_end[clock.segment] = total
    snapshots = _snapshots(track, registers)
    margin: int | None = None
    closest: int | None = None
    gaps: list[WatchdogGap] = []
    for epoch, held in zip(track.epochs, snapshots, strict=True):
        for kind, gap in enumerate(epoch.longest):
            if gap is None:
                continue
            timeout = _reload_timeout(
                held, kind, registers, rates.get(gap.start.segment, []), gap.start
            )
            if timeout is None:
                continue
            left = timeout - gap.ps
            if margin is None or left < margin:
                margin, closest = left, gap.end.pc
            if left <= 0:
                gaps.append(WatchdogGap(gap.start.ps, gap.ps, timeout, gap.start.pc))
    last: int | None = None
    frozen = track.epochs_dropped > 0
    for tail in track.tails:
        if frozen and tail.segment >= track.frozen_segment:
            break
        timeout = _reload_timeout(
            snapshots[tail.epoch],
            tail.kind,
            registers,
            rates.get(tail.segment, []),
            tail.last,
        )
        last = timeout
        if timeout is not None:
            length = segment_end.get(tail.segment, tail.last.ps) - tail.last.ps
            if length >= timeout:
                gaps.append(WatchdogGap(tail.last.ps, length, timeout, tail.last.pc))
    gaps.sort(key=lambda gap: gap.start_ps)
    return Watchdog(
        track.counts[REFRESH_KIND],
        track.counts[START_KIND],
        last,
        margin,
        closest,
        tuple(gaps),
        track.key_writes,
        track.frozen_ps if frozen else None,
    )

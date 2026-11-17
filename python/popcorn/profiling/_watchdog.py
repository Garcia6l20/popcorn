"""The independent watchdog margin, from the logged IWDG register writes."""

from __future__ import annotations

from collections.abc import Callable, Sequence
from dataclasses import dataclass
from fractions import Fraction
from itertools import pairwise

from popcorn import _core
from popcorn._part_description import PartDescription

PS_PER_SECOND = 1_000_000_000_000
REFRESH_KEY = 0xAAAA
START_KEY = 0xCCCC
UNLOCK_KEY = 0x5555
COUNTER_RESET = 0xFFF
"""Count a start reloads, the counter's reset value, RM0456 61.4.3."""
KEY_MASK = 0xFFFF


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
    """A span from a reload with no further reload before the watchdog timeout."""

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
    """Time from the last reload to the expiry the model computes: PR and RLR in force, RL
    periods of the prescaled LSI from the key write, 0xFFF for the start, each part at the LSI
    rate it ran at. None without a reload, with PR or RLR unknown, or with the LSI stopped from
    the reload on."""
    min_margin_ps: int | None
    """Smallest timeout less the time to the next reload, over consecutive reloads of one
    segment. Negative when the watchdog would have expired between them."""
    closest_pc: int | None
    """Address of the reload that ended the smallest margin."""
    gaps: tuple[WatchdogGap, ...]
    """Spans at or past the timeout with no reload, the span after the last reload of each
    segment included."""
    dropped: int
    """Logged writes past the log limit, not counted."""


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


def watchdog(result: _core.ProfileResult, registers: WatchdogRegisters | None) -> Watchdog:
    """The watchdog section of a profile result."""
    if registers is None:
        return Watchdog(0, 0, None, None, None, (), 0)
    kinds = {
        **dict.fromkeys(registers.kr, "kr"),
        **dict.fromkeys(registers.pr, "pr"),
        **dict.fromkeys(registers.rlr, "rlr"),
    }
    logs = [log for log in result.write_logs if log.id in kinds]
    writes = sorted(
        (
            (write.segment, write.record, kinds[log.id], write)
            for log in logs
            for write in log.writes
        ),
        key=lambda item: (item[0], item[1]),
    )
    dropped = sum(log.dropped for log in logs)
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
    return Watchdog(refreshes, starts, last, margin, closest, tuple(gaps), dropped)

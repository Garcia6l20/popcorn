"""The profile report and the recording `Machine.profile` yields."""

from __future__ import annotations

from dataclasses import dataclass
from os import PathLike
from pathlib import Path
from typing import TextIO

from popcorn import _core
from popcorn._observe import ProfileRecording
from popcorn.profiling import _export, _tables
from popcorn.profiling._boot import Milestone, timeline
from popcorn.profiling._symbols import Names
from popcorn.profiling._tables import (
    BURST_WINDOW_PS,
    Analysis,
    ClockSpan,
    Hotspots,
    Irq,
    Loops,
    Peripherals,
    Sleep,
)
from popcorn.profiling._watchdog import Watchdog, WatchdogRegisters, watchdog

NOTES = (
    "loops: a run is passes with the register file and the MMIO read values unchanged at the "
    "head, RAM is not compared; a run reaches to what ended it, so the code from a loop exit to "
    "that event counts in the run; a loop whose inner loop branches back to its own start twice "
    "or more per pass from inside the inner loop reports no run",
    "call tree: a tail call counts in its caller; each process-stack context, an RTOS task, "
    "keeps its own stack; `mov pc, lr` and `bx` through a register other than lr are not seen "
    "as returns, the frame stays until a later return matches deeper; a fault retried at a "
    "block start moves a frame again until the next matching return",
    "watchdog: the timeout is the model's, RL periods of the prescaled LSI from the key write, "
    "0xFFF periods from a start; silicon reloads one prescaled period after the write, RM0456 "
    "61.4.3, so its timeout is RL + 1 periods; each part of the time after a reload counts at "
    "the LSI rate it ran at",
    "watchdog: a PR or RLR write takes effect after a 0x5555 key with no other key since, "
    "masked to its field; a write before the first key of the profile, or after a reset or a "
    "restore, and PR and RLR after a reset or a restore until written, are unknown, and so is "
    "the timeout they give; the update delay of PR and RLR, the window and the early wake-up "
    "are not part of the margin; an IWDG the option bytes start at the reset, IWDG_SW 0 on "
    "the L4, has no start key, so the time from the reset to its first refresh is unchecked",
    "timeline: a milestone is reached when a translated block starts at its address, a "
    "function start or a branch target",
)
"""Limits of the analyses, printed under the report."""

TOP = 20
"""Rows of each table `Report.text` prints."""

MIN_PASSES = 4
"""Fewest identical passes of a loop run the loops analysis reports."""


@dataclass(frozen=True, slots=True)
class Report:
    """What a profile counted, symbolized. Times are picoseconds of virtual time, each span
    converted piecewise with the instruction rate of every clock span it covers, so they hold
    across a clock switch, and every share of time is picoseconds over `elapsed_ps`. A clock span
    rounds down to whole picoseconds: at a rate with no whole picosecond period, 48 MHz for
    example, a time differs from the `Machine.time` delta by less than 1 ps per clock span.
    Instruction counts of the code are in `executed` and the hotspots."""

    analyses: tuple[Analysis, ...]
    """Analyses asked for. A section not asked for is None."""
    start: int
    """Instruction count at the profile start, in `Machine.instructions`' base."""
    from_reset: bool
    """Whether the profile started at the reset, instruction 0. A profile started after a boot
    or from a restored snapshot holds no boot."""
    elapsed_ticks: int
    """Instruction counts of time from the start to the last record, the time across a reset or
    a restore left out: executed instructions, sleeps and held time."""
    elapsed_ps: int
    """Picoseconds from the start to the last record, the sum of the clock spans."""
    executed: int
    """Instructions the core executed."""
    cuts: int
    """Resets and restores inside the profile."""
    clocks: tuple[ClockSpan, ...]
    """Instruction rates, the first at the start, then one per change and per reset or
    restore."""
    hotspots: Hotspots | None
    """Instructions per function."""
    sleep: Sleep | None
    """Sleeps."""
    irq: Irq | None
    """Exceptions."""
    peripherals: Peripherals | None
    """Register accesses."""
    loops: Loops | None
    """Loop runs."""
    watchdog: Watchdog | None
    """Independent watchdog reloads and margins."""
    timeline: tuple[Milestone, ...] | None
    """Milestones asked for, in their order, None when none was."""

    def text(self) -> str:
        """The report as text tables, at most `TOP` rows each, with the limits of the
        analyses."""
        lines = [self._summary()]
        if self.hotspots is not None:
            lines += _hotspot_lines(self.hotspots, self.executed)
        if self.sleep is not None:
            lines += _sleep_lines(self.sleep)
        if self.irq is not None:
            lines += _irq_lines(self.irq)
        if self.peripherals is not None:
            lines += _peripheral_lines(self.peripherals)
        if self.loops is not None:
            lines += _loop_lines(self.loops)
        if self.watchdog is not None:
            lines += _watchdog_lines(self.watchdog)
        if self.timeline is not None:
            lines += _timeline_lines(self.timeline)
        lines += ["", "notes:", *(f"- {note}" for note in NOTES)]
        return "\n".join(lines)

    def to_json(self) -> str:
        """The report as JSON: `{"schema": 1, "report": {...}}`, every field by name, enums by
        value. `from_json` reads it back."""
        return _export.to_json(self)

    @classmethod
    def from_json(cls, text: str) -> Report:
        """The report a `to_json` text holds.

        Raises:
            ValueError: The text is no report of schema 1.
        """
        return _export.from_json(cls, text)

    def write_folded(self, out: str | PathLike[str] | TextIO) -> int:
        """Write the call tree as folded stacks, one `frame;frame;frame count` line per node
        that executed instructions itself, the count its self instructions. flamegraph.pl and
        speedscope read it. Exception frames are `[name]`, a `;` in a name becomes `:`.

        Args:
            out: A path to create, or an open text stream.

        Returns:
            The lines written.

        Raises:
            ValueError: The report has no hotspots section.
        """
        if self.hotspots is None:
            raise ValueError("folded stacks need the hotspots analysis")
        if isinstance(out, (str, PathLike)):
            with Path(out).open("w", encoding="utf-8") as stream:
                return _export.write_folded(self.hotspots.frames, stream)
        return _export.write_folded(self.hotspots.frames, out)

    def _summary(self) -> str:
        rates = ", ".join(f"{clock.hz:,} Hz" for clock in self.clocks) or "no clock"
        origin = (
            "from the reset"
            if self.from_reset
            else f"from instruction {self.start:,}, no boot in it"
        )
        return (
            f"profile {origin}: {_ms(self.elapsed_ps)} at {rates}, "
            f"{self.elapsed_ticks:,} instructions of time, {self.executed:,} executed, "
            f"{self.cuts} cut{'s' if self.cuts != 1 else ''}"
        )


def _ms(ps: int) -> str:
    return f"{ps / 1e9:.3f} ms"


def _us(ps: int) -> str:
    return f"{ps / 1e6:,.1f}"


def _share(part: int, whole: int) -> str:
    return f"{100 * _tables.ratio(part, whole):5.1f}%"


def _hotspot_lines(hotspots: Hotspots, total: int) -> list[str]:
    lines = [
        "",
        "== hotspots ==",
        f"{'function':<36} {'self':>12} {'self%':>6} {'inclusive':>12} {'incl%':>6} {'calls':>9}",
    ]
    lines += [
        f"{row.name[:36]:<36} {row.self:>12,} {_share(row.self, total)} "
        + (
            f"{row.inclusive:>12,} {_share(row.inclusive, total)}"
            if row.inclusive
            else f"{'-':>12} {'-':>6}"
        )
        + f" {row.calls:>9,}"
        for row in hotspots.functions[:TOP]
    ]
    lines.append(
        f"calls {hotspots.calls:,}, returns with no frame {hotspots.unmatched_returns:,}, "
        f"calls past the depth limit {hotspots.deep_calls:,}"
    )
    return lines


def _sleep_lines(sleep: Sleep) -> list[str]:
    reasons = ", ".join(f"{name} {count}" for name, count in sleep.reasons.items() if count)
    lines = [
        "",
        "== sleep ==",
        f"asleep {100 * sleep.share:.1f}% of the time, {sleep.count:,} sleeps, "
        f"{_ms(sleep.time_ps)}, longest {_us(sleep.longest_ps)} us",
        f"by reason: {reasons or 'none'}, cut by the end {sleep.partial}",
        f"held or uncovered: {_ms(sleep.other_gaps_ps)}",
    ]
    lines += [
        f"  woken by {wake.name:<24} {wake.count:>9,} {_ms(wake.time_ps):>14}"
        for wake in sleep.wakes[:TOP]
    ]
    return lines


def _irq_lines(irq: Irq) -> list[str]:
    lines = [
        "",
        "== irq ==",
        f"burst window {_us(irq.burst_window_ps)} us, times in us",
        f"{'exception':<20} {'entries':>9} {'per ms':>8} {'self':>12} {'time':>12} "
        f"{'longest':>9} {'depth':>5} {'tailch':>7} {'burst':>6}",
    ]
    for row in irq.exceptions:
        lines.append(
            f"{row.name[:20]:<20} {row.entries:>9,} {row.per_ms:>8.2f} "
            f"{_us(row.self_time_ps):>12} {_us(row.time_ps):>12} {_us(row.longest_ps):>9} "
            f"{row.max_depth:>5} {row.tail_chained:>7,} {row.burst:>6,}"
        )
        lines += [f"    preempted by {name} x{count}" for name, count in row.preempted_by]
    if irq.unmatched_exits:
        lines.append(f"exits with no entry: {irq.unmatched_exits}")
    return lines


def _peripheral_lines(peripherals: Peripherals) -> list[str]:
    lines = ["", "== peripherals ==", f"{'register':<28} {'reads':>10} {'writes':>10} {'same':>8}"]
    for title, rows in (("cpu", peripherals.cpu), ("dma", peripherals.dma)):
        if rows:
            lines.append(f"{title}:")
            lines += [
                f"  {row.name[:26]:<26} {row.reads:>10,} {row.writes:>10,} {row.same_writes:>8,}"
                for row in rows[:TOP]
            ]
    return lines


def _loop_lines(loops: Loops) -> list[str]:
    lines = [
        "",
        f"== loops (runs of at least {loops.min_passes} identical passes) ==",
        f"{'head':<10} {'where':<32} {'runs':>7} {'passes':>10} {'time':>13} "
        f"{'share':>6}  ended by",
    ]
    for row in loops.loops[:TOP]:
        enders = ", ".join(f"{name} {count}" for name, count in row.enders.items())
        lines.append(
            f"{row.head:#010x} {row.location[:32]:<32} {row.runs:>7,} {row.passes:>10,} "
            f"{_ms(row.time_ps):>13} {100 * row.share:5.1f}%  {enders}"
        )
    return lines


def _watchdog_lines(dog: Watchdog) -> list[str]:
    margin = "-" if dog.min_margin_ps is None else f"{_us(dog.min_margin_ps)} us"
    timeout = "-" if dog.timeout_ps is None else _ms(dog.timeout_ps)
    closest = "" if dog.closest_pc is None else f" at {dog.closest_pc:#010x}"
    lines = [
        "",
        "== watchdog ==",
        f"refreshes {dog.refreshes:,}, starts {dog.starts}, timeout {timeout}, "
        f"smallest margin {margin}{closest}, writes past the log {dog.dropped:,}",
    ]
    lines += [
        f"  no reload for {_ms(gap.length_ps)} from {_ms(gap.start_ps)} "
        f"({gap.pc:#010x}), timeout {_ms(gap.timeout_ps)}"
        for gap in dog.gaps[:TOP]
    ]
    return lines


def _timeline_lines(milestones: tuple[Milestone, ...]) -> list[str]:
    lines = ["", "== timeline =="]
    lines += [
        f"{m.name[:36]:<36} {m.address:#010x} "
        + ("not reached" if m.time_ps is None else _ms(m.time_ps))
        for m in milestones
    ]
    return lines


def build_report(
    result: _core.ProfileResult,
    names: Names,
    analyses: tuple[Analysis, ...],
    *,
    from_reset: bool,
    min_passes: int,
    watchdog_registers: WatchdogRegisters | None = None,
    milestones: tuple[tuple[str, int], ...] | None = None,
) -> Report:
    """The report of a native profile result."""
    return Report(
        analyses,
        result.start,
        from_reset,
        result.elapsed,
        result.elapsed_ps,
        result.instructions,
        result.cuts,
        _tables.clock_spans(result),
        _tables.hotspots(result, names) if Analysis.HOTSPOTS in analyses else None,
        _tables.sleep(result, names) if Analysis.SLEEP in analyses else None,
        _tables.irq(result, names, BURST_WINDOW_PS) if Analysis.IRQ in analyses else None,
        _tables.peripherals(result, names) if Analysis.PERIPHERALS in analyses else None,
        _tables.loops(result, names, min_passes) if Analysis.LOOPS in analyses else None,
        watchdog(result, watchdog_registers) if Analysis.WATCHDOG in analyses else None,
        None if milestones is None else timeline(result, milestones),
    )


class Profile:
    """A profile `Machine.profile` feeds, then its report.

    Leaving the `with` block removes the native sink and takes its counts on the machine's
    thread, at once while parked, at the next block boundary inside a free run: `report()` is
    readable once `closed` is True, after the run halts or once `wait_closed()` returned.
    """

    __slots__ = (
        "_analyses",
        "_from_reset",
        "_milestones",
        "_min_passes",
        "_names",
        "_recording",
        "_report",
        "_watchdog",
    )

    def __init__(
        self,
        recording: ProfileRecording,
        names: Names,
        analyses: tuple[Analysis, ...],
        *,
        from_reset: bool,
        min_passes: int,
        watchdog_registers: WatchdogRegisters | None = None,
        milestones: tuple[tuple[str, int], ...] | None = None,
    ) -> None:
        """Wrap a native profile. `Machine.profile` builds it."""
        self._recording = recording
        self._names = names
        self._analyses = analyses
        self._from_reset = from_reset
        self._min_passes = min_passes
        self._watchdog = watchdog_registers
        self._milestones = milestones
        self._report: Report | None = None

    @property
    def analyses(self) -> tuple[Analysis, ...]:
        """Analyses asked for."""
        return self._analyses

    @property
    def closed(self) -> bool:
        """Whether the counts were taken."""
        return self._recording.closed

    async def wait_closed(self) -> None:
        """Wait for a close posted to a free run. Returns at once when no close is queued."""
        await self._recording.wait_closed()

    def report(self) -> Report:
        """The report, built at the first call.

        Raises:
            MachineRunning: The profile is open, or its close is still queued on a free run.
        """
        if self._report is None:
            self._report = build_report(
                self._recording.result,
                self._names,
                self._analyses,
                from_reset=self._from_reset,
                min_passes=self._min_passes,
                watchdog_registers=self._watchdog,
                milestones=self._milestones,
            )
        return self._report

"""The profile report and the recording `Machine.profile` yields."""

from __future__ import annotations

from dataclasses import dataclass

from popcorn import _core
from popcorn._observe import ProfileRecording
from popcorn.profiling import _tables
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

NOTES = (
    "loops: a run is passes with the register file and the MMIO read values unchanged at the "
    "head, RAM is not compared; a run reaches to what ended it, so the code from a loop exit to "
    "that event counts in the run; a loop whose inner loop branches back to its own start twice "
    "or more per pass from inside the inner loop reports no run",
    "call tree: a tail call counts in its caller; each process-stack context, an RTOS task, "
    "keeps its own stack; `mov pc, lr` and `bx` through a register other than lr are not seen "
    "as returns, the frame stays until a later return matches deeper; a fault retried at a "
    "block start moves a frame again until the next matching return",
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
        lines += ["", "notes:", *(f"- {note}" for note in NOTES)]
        return "\n".join(lines)

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


def build_report(
    result: _core.ProfileResult,
    names: Names,
    analyses: tuple[Analysis, ...],
    *,
    from_reset: bool,
    min_passes: int,
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
        "_min_passes",
        "_names",
        "_recording",
        "_report",
    )

    def __init__(
        self,
        recording: ProfileRecording,
        names: Names,
        analyses: tuple[Analysis, ...],
        *,
        from_reset: bool,
        min_passes: int,
    ) -> None:
        """Wrap a native profile. `Machine.profile` builds it."""
        self._recording = recording
        self._names = names
        self._analyses = analyses
        self._from_reset = from_reset
        self._min_passes = min_passes
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
            )
        return self._report

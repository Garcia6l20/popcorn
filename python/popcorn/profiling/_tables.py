"""Report tables built from a native profile result."""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Iterable
from dataclasses import dataclass
from enum import StrEnum

from popcorn import _core
from popcorn.profiling._symbols import Names
from popcorn.trace import LoopEnder


class Analysis(StrEnum):
    """An analysis `Machine.profile` reports."""

    HOTSPOTS = "hotspots"
    """Instructions per function, self and inclusive, and call counts."""
    SLEEP = "sleep"
    """Sleeps by reason and by the exception that woke the core."""
    IRQ = "irq"
    """Entries, time, nesting and bursts per exception."""
    PERIPHERALS = "peripherals"
    """CPU and DMA accesses per peripheral register."""
    LOOPS = "loops"
    """Runs of identical loop passes, by loop head."""


ALL = tuple(Analysis)
"""Every analysis, the default of `Machine.profile`."""


def selected(analyses: Iterable[Analysis | str]) -> tuple[Analysis, ...]:
    """The analyses named, in `ALL` order, every one when none is named.

    Raises:
        ValueError: A name is no analysis.
    """
    chosen: set[Analysis] = set()
    for analysis in analyses:
        try:
            chosen.add(Analysis(analysis))
        except ValueError:
            known = ", ".join(a.value for a in ALL)
            raise ValueError(f"no analysis {analysis!r}, the analyses are {known}") from None
    return tuple(a for a in ALL if a in chosen) if chosen else ALL


BURST_WINDOW_PS = 1_000_000_000
"""Length of the irq analysis burst window: 1 ms of virtual time, whatever the core rate."""


@dataclass(frozen=True, slots=True)
class ClockSpan:
    """A core instruction rate and the profile time it covered."""

    at: int
    """Instruction count from which the rate held, in the time base of its segment."""
    segment: int
    """Resets and restores seen before it."""
    hz: int
    """Instructions per second."""
    instructions: int
    """Instructions of profile time the rate covered."""
    ps: int
    """Picoseconds the rate covered, `instructions` at `hz` rounded down."""


class FrameKind(StrEnum):
    """Kind of a node of the call tree."""

    ROOT = "root"
    """The root, node 0."""
    ENTRY = "entry"
    """Code reached with no call seen: the first block of the profile, of an exception handler,
    after a reset or a restore, after a return that matched no frame, or of a process-stack
    context resumed with no frames saved for it."""
    CALL = "call"
    """A call."""
    EXCEPTION = "exception"
    """An exception entry."""


@dataclass(frozen=True, slots=True)
class FunctionRow:
    """Instructions of one function."""

    name: str
    """Symbol name, the block address when no function symbol covers it."""
    start: int
    """Function start."""
    self: int
    """Instructions executed in the function itself."""
    inclusive: int
    """Instructions executed in it and in what it called or was interrupted by, from the call
    tree, outermost frames of the function only. 0 when it has no frame of its own: a function
    reached by a branch, a tail call, counts in its caller's frame."""
    calls: int
    """Calls that entered it."""


@dataclass(frozen=True, slots=True)
class FrameRow:
    """A node of the call tree, parents before children."""

    parent: int
    """Index of the parent node, -1 for the root."""
    kind: FrameKind
    """Kind of node."""
    key: int
    """Function start for an entry or a call, exception number for an exception, 0 for the
    root. Names can repeat: the `(kind, key)` pairs on the path from the root identify a node,
    its index identifies it within one report."""
    name: str
    """Function or exception name, empty for the root."""
    entries: int
    """Times the node was entered."""
    self: int
    """Instructions executed in the node itself."""
    inclusive: int
    """Instructions executed in the node and its children."""


@dataclass(frozen=True, slots=True)
class Hotspots:
    """Instructions per function, from block counts and the call tree. Shares are of
    `Report.executed`, the instructions the core executed."""

    functions: tuple[FunctionRow, ...]
    """Functions by self instructions, most first."""
    frames: tuple[FrameRow, ...]
    """Call tree."""
    calls: int
    """Calls that pushed a frame."""
    unmatched_returns: int
    """Returns that matched no frame."""
    deep_calls: int
    """Calls past the depth limit, left in their caller."""


@dataclass(frozen=True, slots=True)
class WakeRow:
    """Sleeps one exception's entry ended."""

    name: str
    """Exception name."""
    count: int
    """Sleeps ended."""
    time_ps: int
    """Picoseconds asleep in them."""


@dataclass(frozen=True, slots=True)
class Sleep:
    """Sleeps, from the core's sleep records."""

    count: int
    """Sleeps."""
    time_ps: int
    """Picoseconds asleep."""
    longest_ps: int
    """Longest sleep in picoseconds."""
    share: float
    """`time_ps` over `Report.elapsed_ps`."""
    reasons: dict[str, int]
    """Sleeps by how the core went to sleep: wfi, wfe, sleep_on_exit, unknown."""
    partial: int
    """Sleeps cut by the end of the profile."""
    wakes: tuple[WakeRow, ...]
    """Sleeps by the exception whose entry ended them, most first."""
    other_gaps_ps: int
    """Picoseconds with no block, exception or sleep record, such as a held core."""


@dataclass(frozen=True, slots=True)
class ExceptionRow:
    """Entries and time of one exception number."""

    number: int
    """Exception number, an interrupt's IRQ number plus 16."""
    name: str
    """Exception name."""
    entries: int
    """Entries."""
    per_ms: float
    """Entries per millisecond of profile time."""
    time_ps: int
    """Picoseconds from entries to their exits, sleeps and preempting exceptions included."""
    self_time_ps: int
    """`time_ps` less the time of the exceptions that preempted it."""
    longest_ps: int
    """Longest time of one entry to its exit in picoseconds."""
    max_depth: int
    """Most exception frames open at one of its entries, itself included."""
    tail_chained: int
    """Entries straight after an exit, with no code between."""
    burst: int
    """Most entries inside one burst window."""
    preempted_by: tuple[tuple[str, int], ...]
    """Entries of other exceptions while it was the innermost frame, by name."""
    cut: int
    """Frames a reset or a restore closed."""
    open: int
    """Frames open at the profile end."""


@dataclass(frozen=True, slots=True)
class Irq:
    """Exceptions, from entry and exit records."""

    exceptions: tuple[ExceptionRow, ...]
    """By number."""
    burst_window_ps: int
    """Length of the burst window in picoseconds, `BURST_WINDOW_PS`."""
    unmatched_exits: int
    """Exits with no matching entry."""


@dataclass(frozen=True, slots=True)
class RegisterRow:
    """Accesses to one peripheral register."""

    name: str
    """`<peripheral>.<register>`, the address when unknown."""
    address: int
    """Register address from the description, the address of the first access when the
    register is not in it."""
    reads: int
    """Reads."""
    writes: int
    """Writes."""
    same_writes: int
    """Writes of the value the previous write stored."""
    last_value: int
    """Value of the last access."""


@dataclass(frozen=True, slots=True)
class Peripherals:
    """Peripheral register accesses by initiator. Debugger accesses, from Python or a probe,
    are left out."""

    cpu: tuple[RegisterRow, ...]
    """CPU accesses, most first."""
    dma: tuple[RegisterRow, ...]
    """DMA accesses, most first."""


@dataclass(frozen=True, slots=True)
class LoopRow:
    """Runs of identical passes of one loop."""

    head: int
    """Loop head address."""
    location: str
    """`function+offset` of the head."""
    last: int
    """Highest block start of the loop."""
    runs: int
    """Runs reported."""
    passes: int
    """Identical passes over the runs."""
    time_ps: int
    """Picoseconds the runs covered."""
    share: float
    """`time_ps` over `Report.elapsed_ps`."""
    enders: dict[str, int]
    """Runs by what ended them, LoopEnder names in lower case."""


@dataclass(frozen=True, slots=True)
class Loops:
    """Loop runs of at least `min_passes` identical passes."""

    loops: tuple[LoopRow, ...]
    """By time, most first."""
    min_passes: int
    """Fewest passes of a reported run."""


def ratio(part: int, whole: int) -> float:
    """part / whole, 0.0 for an empty whole."""
    return part / whole if whole else 0.0


def clock_spans(result: _core.ProfileResult) -> tuple[ClockSpan, ...]:
    """The instruction rates of `result` with the time each covered."""
    return tuple(
        ClockSpan(clock.at, clock.segment, clock.hz, clock.instructions, clock.ps)
        for clock in result.clocks
    )


def hotspots(result: _core.ProfileResult, names: Names) -> Hotspots:
    """Self instructions per function from the block counts, inclusive and calls from the call
    tree."""
    self_counts: dict[int, int] = defaultdict(int)
    labels: dict[int, str] = {}
    for block in result.code:
        name, start = names.function(block.pc)
        self_counts[start] += block.instructions
        labels[start] = name
    nodes = result.frames
    frames: list[FrameRow] = []
    inclusive: dict[int, int] = defaultdict(int)
    calls: dict[int, int] = defaultdict(int)
    outermost: list[bool] = []
    ancestors: list[frozenset[tuple[str, int]]] = []
    for index, node in enumerate(nodes):
        kind = FrameKind(node.kind.name)
        key = (kind if kind is FrameKind.EXCEPTION else "code", node.key)
        above = ancestors[node.parent] if index else frozenset[tuple[str, int]]()
        ancestors.append(above | {key} if index else above)
        outermost.append(index != 0 and key not in above)
        if kind is FrameKind.EXCEPTION:
            label = names.exception(node.key)
        elif kind is FrameKind.ROOT:
            label = ""
        else:
            label = names.function_at(node.key)
            labels.setdefault(node.key, label)
            if outermost[index]:
                inclusive[node.key] += node.inclusive
            if kind is FrameKind.CALL:
                calls[node.key] += node.entries
        parent = node.parent if index else -1
        frames.append(
            FrameRow(parent, kind, node.key, label, node.entries, node.self, node.inclusive)
        )
    starts = set(self_counts) | set(inclusive)
    functions = sorted(
        (
            FunctionRow(labels[start], start, self_counts[start], inclusive[start], calls[start])
            for start in starts
        ),
        key=lambda f: (-f.self, -f.inclusive, f.start),
    )
    return Hotspots(
        tuple(functions),
        tuple(frames),
        result.calls,
        result.unmatched_returns,
        result.deep_calls,
    )


SLEEP_REASONS = ("unknown", "wfi", "wfe", "sleep_on_exit")


def sleep(result: _core.ProfileResult, names: Names) -> Sleep:
    """Sleeps by reason and by waking exception."""
    sleeps = result.sleeps
    wakes = sorted(
        (
            WakeRow(names.exception(number), stats.wakes.count, stats.wakes.total_ps)
            for number, stats in result.exceptions.items()
            if stats.wakes.count
        ),
        key=lambda w: (-w.count, w.name),
    )
    return Sleep(
        sleeps.count,
        sleeps.total_ps,
        sleeps.longest_ps,
        ratio(sleeps.total_ps, result.elapsed_ps),
        dict(zip(SLEEP_REASONS, result.sleep_reasons, strict=True)),
        result.partial_sleeps,
        tuple(wakes),
        result.other_gaps.total_ps,
    )


def irq(result: _core.ProfileResult, names: Names, burst_window_ps: int) -> Irq:
    """Exceptions by number."""
    rows = tuple(
        ExceptionRow(
            number,
            names.exception(number),
            stats.entries,
            ratio(stats.entries * 1_000_000_000, result.elapsed_ps),
            stats.inclusive_ps,
            stats.self_ps,
            stats.longest_ps,
            stats.max_depth,
            stats.tail_chained,
            stats.burst,
            tuple((names.exception(n), count) for n, count in stats.preempted_by.items()),
            stats.cut,
            stats.open,
        )
        for number, stats in result.exceptions.items()
        if stats.entries or stats.open or stats.cut
    )
    return Irq(rows, burst_window_ps, result.unmatched_exits)


def _registers(rows: list[_core.ProfileRegister], names: Names) -> tuple[RegisterRow, ...]:
    built = (
        RegisterRow(
            names.register(row.id, row.first_address),
            names.register_address(row.id, row.first_address),
            row.reads,
            row.writes,
            row.same_writes,
            row.last_value,
        )
        for row in rows
    )
    return tuple(sorted(built, key=lambda r: (-(r.reads + r.writes), r.name)))


def peripherals(result: _core.ProfileResult, names: Names) -> Peripherals:
    """CPU and DMA register accesses."""
    return Peripherals(
        _registers(result.cpu_registers, names), _registers(result.dma_registers, names)
    )


def loops(result: _core.ProfileResult, names: Names, min_passes: int) -> Loops:
    """Loop runs by head."""
    rows = (
        LoopRow(
            loop.head,
            names.location(loop.head),
            loop.last,
            loop.runs,
            loop.passes,
            loop.ps,
            ratio(loop.ps, result.elapsed_ps),
            {LoopEnder(i).name.lower(): count for i, count in enumerate(loop.enders) if count},
        )
        for loop in result.loops
    )
    return Loops(tuple(sorted(rows, key=lambda row: (-row.time_ps, row.head))), min_passes)

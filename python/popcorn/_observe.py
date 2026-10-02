"""Coverage, full traces, the trace ring and log capture of one machine."""

from __future__ import annotations

import logging
import os
from collections.abc import Callable, Iterable, Sequence
from pathlib import Path

from popcorn import _core
from popcorn._enums import StopReason
from popcorn._image import Image, Location
from popcorn.coverage import Coverage
from popcorn.trace import Block, Call, Irq, Mem, Mmio, Record, Stop, TraceKind, read

_LOG = logging.getLogger("popcorn")

type Ranges = Iterable[tuple[int, int]]
"""Code ranges as (first address, size in bytes)."""


def _rows(ranges: Ranges | None) -> list[tuple[str, int, int]]:
    return [(f"{start:#010x}", start, size) for start, size in ranges or ()]


class MachineCoverage:
    """Block and edge coverage of one machine, `m.coverage`.

    Counting runs natively, no Python call per block. Counters are cumulative across restores
    and kept by `disable()`, zeroed by `clear()`.
    """

    __slots__ = ("_check", "_handle", "_images")

    def __init__(
        self,
        handle: _core.MachineHandle,
        images: Callable[[], Sequence[Image]],
        check: Callable[[], None],
    ) -> None:
        """Wrap a machine. `Machine.coverage` builds it."""
        self._handle = handle
        self._images = images
        self._check = check

    def enable(self, ranges: Ranges | None = None, functions: Iterable[str] | None = None) -> None:
        """Start counting over `ranges` and the code of `functions`, every executable segment
        of the loaded images when both are None. Enabling again adds ranges.

        Raises:
            SymbolError: No loaded image has a function name.
            ConfigError: A range is empty or leaves the 32-bit address space.
            MachineRunning: A run is active.
        """
        self._check()
        rows = _rows(ranges)
        if functions is not None:
            rows += self._handle.function_ranges(list(functions))
        if not rows and (ranges is not None or functions is not None):
            return
        self._handle.enable_coverage(rows)

    def disable(self) -> None:
        """Stop counting, keeping the counters."""
        self._check()
        self._handle.disable_coverage()

    def clear(self) -> None:
        """Zero every counter, ranges kept."""
        self._check()
        self._handle.clear_coverage()

    @property
    def enabled(self) -> bool:
        """Whether coverage is being counted."""
        return self._handle.coverage_enabled()

    def collect(self) -> Coverage:
        """The coverage counted so far, one group over the loaded images.

        ELF images are recorded by absolute path and digest, others as raw images.
        """
        self._check()
        sources = [
            os.path.abspath(image.path) if image.is_elf and image.path is not None else ""
            for image in self._images()
        ]
        return Coverage(self._handle.coverage_pcov(sources))

    def save(self, path: str | os.PathLike[str]) -> Coverage:
        """Write the coverage so far to a `.pcov` file and return it."""
        coverage = self.collect()
        coverage.save(path)
        return coverage

    def merge_into(self, path: str | os.PathLike[str]) -> Coverage:
        """Add the coverage so far to a `.pcov` file, a worker's file, and return the sum."""
        return self.collect().merge_into(path)


class TraceRecording:
    """A `.pctr` trace being written, from `Machine.trace`."""

    __slots__ = ("_native", "path")

    def __init__(self, native: _core.TraceHandle, path: Path) -> None:
        """Wrap a native trace. `Machine.trace` builds it."""
        self._native = native
        self.path = path

    @property
    def records(self) -> int:
        """Records written so far, name and time records included."""
        return self._native.records()

    @property
    def size(self) -> int:
        """Bytes written so far, the file size once closed."""
        return self._native.size()

    def close(self) -> None:
        """Stop tracing and close the file, what leaving the `with` block does.

        Raises:
            FormatError: A write, the flush or the close failed, `popcorn._obs.FormatError`.
        """
        self._native.close()


def open_trace(
    handle: _core.MachineHandle,
    kinds: Iterable[TraceKind],
    path: str | os.PathLike[str],
    ranges: Ranges | None,
    functions: Iterable[str] | None,
) -> TraceRecording:
    """Start a trace for `Machine.trace`."""
    rows = _rows(ranges)
    if functions is not None:
        rows += handle.function_ranges(list(functions))
    target = Path(path)
    native = handle.open_trace(os.fspath(target), sorted({int(k) for k in kinds}), rows)
    return TraceRecording(native, target)


class Ring:
    """The always-on trace ring of a machine, `m.ring`: its most recent records."""

    __slots__ = ("_check", "_handle", "_idle", "_locate")

    def __init__(
        self,
        handle: _core.MachineHandle,
        locate: Callable[[int], Location],
        check: Callable[[], None],
        idle: Callable[[], None],
    ) -> None:
        """Wrap a machine. `Machine.ring` builds it."""
        self._handle = handle
        self._locate = locate
        self._check = check
        self._idle = idle

    def clear(self) -> None:
        """Drop every kept record. The next records count their time from now.

        Raises:
            MachineRunning: A run is active, handlers of it included.
        """
        self._idle()
        self._handle.clear_ring()

    def records(self) -> list[Record]:
        """The kept records, oldest first, registers and symbols named."""
        import tempfile

        self._check()
        with tempfile.TemporaryDirectory(prefix="popcorn-ring-") as directory:
            path = Path(directory) / "ring.pctr"
            self._handle.write_ring(os.fspath(path))
            return list(read(path))

    def dump(self, *, last: int | None = None, collapse: bool = False) -> str:
        """The kept records, one line each: instruction count, kind, symbolized pc, detail.

        Args:
            last: Keep only the last `last` lines, after a first line counting those left out.
                None keeps every line.
            collapse: Print a group of up to 8 records repeated back to back, times aside, once,
                then one line with the repeat count and the time of the last repeat. A poll
                loop then takes a few lines instead of the whole ring.

        Raises:
            ValueError: `last` is below 1.
        """
        if last is not None and last < 1:
            raise ValueError(f"last must be at least 1, not {last}")
        lines = [(record.time, self._body(record)) for record in self.records()]
        text = collapsed(lines) if collapse else [f"{time:>12} {body}" for time, body in lines]
        if last is not None and len(text) > last:
            hidden = len(text) - last
            text = [f"... {hidden} earlier line{'s' if hidden != 1 else ''}", *text[-last:]]
        return "\n".join(text)

    def _body(self, record: Record) -> str:
        match record:
            case Block(_, pc, icount):
                return f"block {self._locate(pc)} {icount} insn"
            case Mem(_, pc, address, value, size, write):
                access = f"{'write' if write else 'read'}{size * 8} {address:#010x}"
                return f"mem   {access} = {value:#x} at {self._locate(pc)}"
            case Mmio(_, pc, address, value, size, write, register):
                access = (
                    f"{'write' if write else 'read'}{size * 8} {register or f'{address:#010x}'}"
                )
                return f"mmio  {access} = {value:#x} at {self._locate(pc)}"
            case Irq(_, number, enter):
                return f"irq   {'enter' if enter else 'exit'} {number}"
            case Call(_, pc, r0, symbol):
                return f"call  {symbol or self._locate(pc)} r0={r0:#x}"
            case Stop(_, pc, reason):
                return f"stop  {StopReason(reason).name} at {self._locate(pc)}"


_PERIOD = 8


def collapsed(lines: Sequence[tuple[int, str]]) -> list[str]:
    """`lines` of (time, text) as `Ring.dump(collapse=True)` prints them. At each line, the
    period of up to 8 lines whose back-to-back repeats cover the most lines wins, the shorter on
    a tie."""
    bodies = [body for _, body in lines]
    out: list[str] = []
    i = 0
    while i < len(lines):
        best_period, best_repeats = 0, 1
        for period in range(1, _PERIOD + 1):
            if i + 2 * period > len(lines):
                break
            group = bodies[i : i + period]
            repeats = 1
            while bodies[i + repeats * period : i + (repeats + 1) * period] == group:
                repeats += 1
            if repeats > 1 and repeats * period > best_repeats * best_period:
                best_period, best_repeats = period, repeats
        if best_period == 0:
            out.append(f"{lines[i][0]:>12} {bodies[i]}")
            i += 1
            continue
        out.extend(f"{time:>12} {body}" for time, body in lines[i : i + best_period])
        i += best_period * best_repeats
        more = best_repeats - 1
        what = "record" if best_period == 1 else f"{best_period} records"
        out.append(
            f"{'':>12} ... {what} repeated {more} more time{'s' if more != 1 else ''}, "
            f"last at {lines[i - best_period][0]}"
        )
    return out


def forward_log(part: str, record: _core.LogRecord) -> None:
    """Send a machine log record to the `popcorn` logger at WARNING, its fields as extras."""
    _LOG.warning(
        "%s: %s",
        part,
        record.message,
        extra={
            "popcorn_kind": record.kind.name,
            "popcorn_peripheral": record.peripheral,
            "popcorn_register": record.reg,
            "popcorn_offset": record.offset,
            "popcorn_pc": record.pc,
            "popcorn_write": record.write,
        },
    )

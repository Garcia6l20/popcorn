"""Chrome trace JSON export of .pctr files, for Perfetto and chrome://tracing."""

from __future__ import annotations

import json
import os
from collections.abc import Collection
from pathlib import Path
from typing import Final, TextIO

from popcorn.trace._reader import read
from popcorn.trace._records import (
    ALL,
    Block,
    Call,
    Clock,
    Cut,
    Irq,
    Mem,
    Mmio,
    Rebase,
    Record,
    Sleep,
    Stop,
    TraceKind,
)

type PathLike = str | os.PathLike[str]
type Event = dict[str, object]

_PID: Final = 1
_THREADS: Final = {
    TraceKind.BLOCK: (1, "blocks"),
    TraceKind.MEM: (2, "memory"),
    TraceKind.MMIO: (3, "mmio"),
    TraceKind.IRQ: (4, "irq"),
    TraceKind.CALL: (5, "calls"),
    TraceKind.STOP: (6, "stops"),
    TraceKind.SLEEP: (7, "sleep"),
    TraceKind.CLOCK: (8, "clock"),
    TraceKind.CUT: (1, "blocks"),
}
_REASONS: Final = ("sleep", "WFI", "WFE", "sleep on exit")
_KIND: Final[dict[type[Record], TraceKind]] = {
    Block: TraceKind.BLOCK,
    Mem: TraceKind.MEM,
    Mmio: TraceKind.MMIO,
    Irq: TraceKind.IRQ,
    Call: TraceKind.CALL,
    Stop: TraceKind.STOP,
    Sleep: TraceKind.SLEEP,
    Clock: TraceKind.CLOCK,
    Cut: TraceKind.CUT,
    Rebase: TraceKind.STOP,
}


def _instant(record: Record, name: str, args: dict[str, object], scope: str = "t") -> Event:
    return {
        "name": name,
        "ph": "i",
        "s": scope,
        "ts": record.time,
        "pid": _PID,
        "tid": _THREADS[_KIND[type(record)]][0],
        "args": args,
    }


def _irq_event(number: int, enter: bool, time: int, frame: int = 0) -> Event:
    event: Event = {
        "name": f"IRQ {number}",
        "ph": "B" if enter else "E",
        "ts": time,
        "pid": _PID,
        "tid": _THREADS[TraceKind.IRQ][0],
    }
    if frame:
        event["args"] = {"frame": frame}
    return event


def _events(record: Record, open_irqs: dict[int, int]) -> list[Event]:
    match record:
        case Block():
            return [
                {
                    "name": f"0x{record.pc:08x}",
                    "ph": "X",
                    "ts": record.time - record.icount,
                    "dur": record.icount,
                    "pid": _PID,
                    "tid": _THREADS[TraceKind.BLOCK][0],
                }
            ]
        case Mmio():
            args: dict[str, object] = {
                "pc": record.pc,
                "address": record.address,
                "value": record.value,
                "size": record.size,
                "write": record.write,
                "register": record.register,
            }
            return [_instant(record, record.register or f"0x{record.address:08x}", args)]
        case Mem():
            args = {
                "pc": record.pc,
                "address": record.address,
                "value": record.value,
                "size": record.size,
                "write": record.write,
            }
            return [_instant(record, "write" if record.write else "read", args)]
        case Irq():
            if not record.enter:
                if open_irqs.get(record.number, 0) == 0:
                    return []
                open_irqs[record.number] -= 1
            else:
                open_irqs[record.number] = open_irqs.get(record.number, 0) + 1
            return [_irq_event(record.number, record.enter, record.time, record.frame)]
        case Call():
            name = record.symbol or f"0x{record.pc:08x}"
            return [_instant(record, name, {"pc": record.pc, "r0": record.r0})]
        case Stop():
            return [_instant(record, f"stop {record.reason}", {"pc": record.pc}, scope="g")]
        case Sleep():
            reason = _REASONS[record.reason] if record.reason < len(_REASONS) else "sleep"
            return [
                {
                    "name": reason,
                    "ph": "X",
                    "ts": record.time - record.length,
                    "dur": record.length,
                    "pid": _PID,
                    "tid": _THREADS[TraceKind.SLEEP][0],
                    "args": {"woke_by": record.woke_by, "partial": record.partial},
                }
            ]
        case Clock():
            return [_instant(record, f"{record.hz} Hz", {"hz": record.hz})]
        case Cut():
            return [_instant(record, "cut", {"pc": record.pc, "unrun": record.unrun})]
        case Rebase():
            return [_instant(record, "rebase", {}, scope="g")]


def _close(open_irqs: dict[int, int], time: int) -> list[Event]:
    closing = [
        _irq_event(number, False, time)
        for number, depth in sorted(open_irqs.items())
        for _ in range(depth)
    ]
    open_irqs.clear()
    return closing


def _metadata(kind: str, tid: int | None, name: str) -> Event:
    event: Event = {"name": kind, "ph": "M", "pid": _PID, "args": {"name": name}}
    if tid is not None:
        event["tid"] = tid
    return event


def _write(out: TextIO, events: list[Event], first: bool) -> bool:
    for event in events:
        out.write("" if first else ",\n")
        out.write(json.dumps(event, separators=(",", ":")))
        first = False
    return first


def to_chrome(src: PathLike, dst: PathLike, *, kinds: Collection[TraceKind] | None = None) -> None:
    """Converts a .pctr file to Chrome trace JSON, one event per record, streamed.

    `ts` is the instruction count: one microsecond in the viewer is one instruction. Blocks and
    sleeps are complete events, IRQs begin and end events on their own thread, with the process
    stack frame address as the `frame` arg when it is not 0, the rest instant events. A rebase, a
    reset or a restore, is always written as a global instant. An IRQ exit with no open entry,
    from a trace started inside a handler, is dropped, and an IRQ still open at the end of the
    file or at a rebase is closed at the time of the record before.
    The JSON is written to a temporary file renamed over dst once complete.

    Args:
        src: The .pctr file, raw or zstd-compressed.
        dst: The JSON file to write.
        kinds: Kinds to convert, every kind by default.

    Raises:
        TraceFileError: the .pctr file is malformed.
    """
    selected = frozenset(ALL if kinds is None else kinds)
    open_irqs: dict[int, int] = {}
    target = Path(dst)
    temporary = target.with_name(target.name + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8") as out:
            out.write('{"traceEvents":[\n')
            metadata = [_metadata("process_name", None, "popcorn")]
            metadata += [
                _metadata("thread_name", tid, name) for tid, name in dict(_THREADS.values()).items()
            ]
            first = _write(out, metadata, True)
            last = 0
            for record in read(src):
                if isinstance(record, Rebase):
                    first = _write(out, _close(open_irqs, last), first)
                last = record.time
                if isinstance(record, Rebase) or _KIND[type(record)] in selected:
                    first = _write(out, _events(record, open_irqs), first)
            _write(out, _close(open_irqs, last), first)
            out.write('\n],"otherData":{"clock":"instructions"}}\n')
        temporary.replace(target)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

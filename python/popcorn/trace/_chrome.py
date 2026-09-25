"""Chrome trace JSON export of .pctr files, for Perfetto and chrome://tracing."""

from __future__ import annotations

import json
import os
from collections.abc import Collection
from pathlib import Path
from typing import Final, TextIO

from popcorn.trace._reader import read
from popcorn.trace._records import ALL, Block, Call, Irq, Mem, Mmio, Record, Stop, TraceKind

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
}
_KIND: Final[dict[type[Record], TraceKind]] = {
    Block: TraceKind.BLOCK,
    Mem: TraceKind.MEM,
    Mmio: TraceKind.MMIO,
    Irq: TraceKind.IRQ,
    Call: TraceKind.CALL,
    Stop: TraceKind.STOP,
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


def _irq_event(number: int, enter: bool, time: int) -> Event:
    return {
        "name": f"IRQ {number}",
        "ph": "B" if enter else "E",
        "ts": time,
        "pid": _PID,
        "tid": _THREADS[TraceKind.IRQ][0],
    }


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
            return [_irq_event(record.number, record.enter, record.time)]
        case Call():
            name = record.symbol or f"0x{record.pc:08x}"
            return [_instant(record, name, {"pc": record.pc, "r0": record.r0})]
        case Stop():
            return [_instant(record, f"stop {record.reason}", {"pc": record.pc}, scope="g")]


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

    `ts` is the instruction count: one microsecond in the viewer is one instruction. Blocks are
    complete events, IRQs begin and end events on their own thread, the rest instant events. An
    IRQ exit with no open entry, from a trace started inside a handler, is dropped, and an IRQ
    still open at the end of the file is closed at the last record's time. The JSON is written to
    a temporary file renamed over dst once complete.

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
            metadata += [_metadata("thread_name", tid, name) for tid, name in _THREADS.values()]
            first = _write(out, metadata, True)
            last = 0
            for record in read(src):
                last = record.time
                if _KIND[type(record)] in selected:
                    first = _write(out, _events(record, open_irqs), first)
            closing = [
                _irq_event(number, False, last)
                for number, depth in sorted(open_irqs.items())
                for _ in range(depth)
            ]
            _write(out, closing, first)
            out.write('\n],"otherData":{"clock":"instructions"}}\n')
        temporary.replace(target)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

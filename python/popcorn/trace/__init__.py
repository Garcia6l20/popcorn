"""Full traces of popcorn machines: .pctr files, typed reader, Chrome trace JSON export."""

from popcorn.trace._chrome import to_chrome
from popcorn.trace._reader import TraceFileError, compress, read
from popcorn.trace._records import (
    ALL,
    Block,
    Call,
    Clock,
    Cut,
    Initiator,
    Irq,
    Mem,
    Mmio,
    Rebase,
    Record,
    Sleep,
    Stop,
    TraceKind,
)

__all__ = [
    "ALL",
    "Block",
    "Call",
    "Clock",
    "Cut",
    "Initiator",
    "Irq",
    "Mem",
    "Mmio",
    "Rebase",
    "Record",
    "Sleep",
    "Stop",
    "TraceFileError",
    "TraceKind",
    "compress",
    "read",
    "to_chrome",
]

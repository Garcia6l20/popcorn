"""Full traces of popcorn machines: .pctr files, typed reader, Chrome trace JSON export."""

from popcorn.trace._chrome import to_chrome
from popcorn.trace._reader import TraceFileError, compress, read
from popcorn.trace._records import (
    ALL,
    Block,
    Call,
    Irq,
    Mem,
    Mmio,
    Record,
    Stop,
    TraceKind,
)

__all__ = [
    "ALL",
    "Block",
    "Call",
    "Irq",
    "Mem",
    "Mmio",
    "Record",
    "Stop",
    "TraceFileError",
    "TraceKind",
    "compress",
    "read",
    "to_chrome",
]

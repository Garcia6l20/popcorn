"""Typed records of a .pctr trace file."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from enum import IntEnum
from types import MappingProxyType
from typing import Final, Literal


class TraceKind(IntEnum):
    """Kind byte of a .pctr record."""

    BLOCK = 0
    MEM = 1
    MMIO = 2
    IRQ = 3
    CALL = 4
    STR = 5
    TIME = 6
    STOP = 7
    SLEEP = 8
    CLOCK = 9
    CUT = 10
    LOOP = 11


ALL: Final[frozenset[TraceKind]] = frozenset(
    kind for kind in TraceKind if kind not in (TraceKind.STR, TraceKind.TIME)
)
"""Every kind a trace sink can select: STR and TIME exist only in files."""

PAYLOAD_WORDS: Final[Mapping[TraceKind, int]] = MappingProxyType(
    {
        TraceKind.BLOCK: 1,
        TraceKind.MEM: 3,
        TraceKind.MMIO: 3,
        TraceKind.IRQ: 1,
        TraceKind.CALL: 2,
        TraceKind.STR: 0,
        TraceKind.TIME: 2,
        TraceKind.STOP: 1,
        TraceKind.SLEEP: 1,
        TraceKind.CLOCK: 2,
        TraceKind.CUT: 1,
        TraceKind.LOOP: 3,
    }
)
"""u32 payload words after each record header, by kind."""


@dataclass(frozen=True, slots=True)
class Block:
    """A translated block ran.

    Stamped after the block's instructions were counted, so it ran over the instruction counts
    [time - icount, time).
    """

    time: int
    pc: int
    icount: int


@dataclass(frozen=True, slots=True)
class Mem:
    """A memory access."""

    time: int
    pc: int
    address: int
    value: int
    size: int
    write: bool


type Initiator = Literal["cpu", "dma", "debugger"]
"""Who made a register access."""


@dataclass(frozen=True, slots=True)
class Mmio:
    """A peripheral register access, the register named `<peripheral>.<register>` when known.

    pc is 0 for a DMA or debugger access.
    """

    time: int
    pc: int
    address: int
    value: int
    size: int
    write: bool
    register: str | None
    initiator: Initiator = "cpu"


@dataclass(frozen=True, slots=True)
class Irq:
    """An exception entry or exit.

    ``frame`` is the address of the exception frame on the process stack when the entry comes
    from, or the exit returns to, thread mode on the process stack, the same word for one
    suspended context. It is 0 for every other entry and exit.
    """

    time: int
    number: int
    enter: bool
    frame: int = 0


@dataclass(frozen=True, slots=True)
class Call:
    """An intercepted call, the function named when known."""

    time: int
    pc: int
    r0: int
    symbol: str | None


@dataclass(frozen=True, slots=True)
class Stop:
    """The machine stopped, reason the stop_reason value."""

    time: int
    pc: int
    reason: int


@dataclass(frozen=True, slots=True)
class Sleep:
    """The core slept over the instruction counts [time - length, time) and woke at time.

    A sleep that began before the trace's origin or its last rebase starts there.

    reason is how it went to sleep: 0 unknown, 1 WFI, 2 WFE, 3 sleep on exit. woke_by is the
    exception whose entry woke it, 0 for a wake with no entry. partial marks a sleep cut short by
    the end of the trace.
    """

    time: int
    length: int
    reason: int
    woke_by: int
    partial: bool


@dataclass(frozen=True, slots=True)
class Clock:
    """The core runs hz instructions per second from time on."""

    time: int
    hz: int


@dataclass(frozen=True, slots=True)
class Cut:
    """The block that started at pc and was recorded last was left with unrun of its instructions
    not run."""

    time: int
    pc: int
    unrun: int


class LoopEnder(IntEnum):
    """What ended a run of loop passes."""

    PROGRESS = 0
    """A head arrival found a pass different from the one before, a back edge took another head,
    or a handler returned."""
    EXCEPTION = 1
    """An exception was entered."""
    SLEEP = 2
    """The core went to sleep or was held."""
    END = 3
    """Loop reports were turned off or the sink was removed."""
    REBASE = 4
    """A reset or a restore moved the instruction count."""
    CALL = 5
    """A host call started, or ended with a run of its own still open."""


@dataclass(frozen=True, slots=True)
class Loop:
    """A run of identical passes of the loop at head, over the instruction counts
    [time - instructions, time) while instructions is below the 0xffffffff cap.

    A pass runs from head back to it with the full register file and every MMIO read value equal
    to the pass before, RAM not compared. A run ends at a head arrival, a back edge or another
    ender, so a run whose code left the loop on its last pass also covers the code from the exit
    to time. instructions starts no earlier than the trace's first record or its last rebase.
    last is the highest block start of the loop, below head + 256. exception is the exception
    number for an EXCEPTION ender, 0 otherwise.
    """

    time: int
    head: int
    last: int
    passes: int
    instructions: int
    ender: LoopEnder
    exception: int


@dataclass(frozen=True, slots=True)
class Rebase:
    """A reset or a restore moved the instruction count to time. Records after it count from
    there, nothing before it carries over."""

    time: int


type Record = Block | Mem | Mmio | Irq | Call | Stop | Sleep | Clock | Cut | Loop | Rebase

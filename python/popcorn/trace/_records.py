"""Typed records of a .pctr trace file."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from enum import IntEnum
from types import MappingProxyType
from typing import Final


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


ALL: Final[frozenset[TraceKind]] = frozenset(
    kind for kind in TraceKind if kind not in (TraceKind.STR, TraceKind.TIME)
)
"""Every kind a trace sink can select: STR and TIME exist only in files."""

PAYLOAD_WORDS: Final[Mapping[TraceKind, int]] = MappingProxyType(
    {
        TraceKind.BLOCK: 1,
        TraceKind.MEM: 3,
        TraceKind.MMIO: 3,
        TraceKind.IRQ: 0,
        TraceKind.CALL: 2,
        TraceKind.STR: 0,
        TraceKind.TIME: 2,
        TraceKind.STOP: 1,
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


@dataclass(frozen=True, slots=True)
class Mmio:
    """A peripheral register access, the register named `<peripheral>.<register>` when known."""

    time: int
    pc: int
    address: int
    value: int
    size: int
    write: bool
    register: str | None


@dataclass(frozen=True, slots=True)
class Irq:
    """An exception entry or exit."""

    time: int
    number: int
    enter: bool


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


type Record = Block | Mem | Mmio | Irq | Call | Stop

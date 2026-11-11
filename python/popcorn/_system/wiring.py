from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    pass


@dataclass(frozen=True)
class LinkWires:
    ends: tuple[str, str]
    wires: tuple[int, int]
    link: int


@dataclass(frozen=True)
class SpiWired:
    ends: tuple[str, ...]
    selects: tuple[str | None, ...]
    bus: int


@dataclass(frozen=True)
class I2CWired:
    ends: tuple[str, ...]
    bus: int


type Wired = LinkWires | SpiWired | I2CWired

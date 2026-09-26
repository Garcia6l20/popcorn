"""Virtual time, durations and frequencies, in exact integers.

`VirtualTime` and `VirtualDuration` count picoseconds of the machine's virtual clock, never wall
time. `asyncio.timeout()` is the wall-clock bound.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import overload

_PS_PER_SECOND = 10**12


def _scaled(value: float, unit: int) -> int:
    if isinstance(value, int):
        return value * unit
    return round(value * unit)


@dataclass(frozen=True, order=True, slots=True)
class VirtualDuration:
    """A span of virtual time, in picoseconds."""

    ps: int

    @property
    def seconds(self) -> float:
        """The duration in seconds, as a float."""
        return self.ps / _PS_PER_SECOND

    def __add__(self, other: VirtualDuration) -> VirtualDuration:
        return VirtualDuration(self.ps + other.ps)

    def __sub__(self, other: VirtualDuration) -> VirtualDuration:
        return VirtualDuration(self.ps - other.ps)

    def __mul__(self, factor: int) -> VirtualDuration:
        return VirtualDuration(self.ps * factor)

    def __rmul__(self, factor: int) -> VirtualDuration:
        return VirtualDuration(self.ps * factor)

    def __floordiv__(self, divisor: int) -> VirtualDuration:
        return VirtualDuration(self.ps // divisor)

    def __str__(self) -> str:
        return f"{self.ps} ps"


@dataclass(frozen=True, order=True, slots=True)
class VirtualTime:
    """A point of virtual time since the last reset, in picoseconds."""

    ps: int

    @property
    def seconds(self) -> float:
        """The time in seconds since reset, as a float."""
        return self.ps / _PS_PER_SECOND

    def __add__(self, other: VirtualDuration) -> VirtualTime:
        return VirtualTime(self.ps + other.ps)

    @overload
    def __sub__(self, other: VirtualTime) -> VirtualDuration: ...
    @overload
    def __sub__(self, other: VirtualDuration) -> VirtualTime: ...
    def __sub__(self, other: VirtualTime | VirtualDuration) -> VirtualDuration | VirtualTime:
        if isinstance(other, VirtualTime):
            return VirtualDuration(self.ps - other.ps)
        return VirtualTime(self.ps - other.ps)

    def __str__(self) -> str:
        return f"t={self.ps} ps"


@dataclass(frozen=True, order=True, slots=True)
class Frequency:
    """A clock rate, in hertz."""

    hz: int

    @property
    def period(self) -> VirtualDuration:
        """Duration of one cycle, rounded down to a picosecond."""
        return VirtualDuration(_PS_PER_SECOND // self.hz)


def s(value: float) -> VirtualDuration:
    """`value` seconds of virtual time."""
    return VirtualDuration(_scaled(value, _PS_PER_SECOND))


def ms(value: float) -> VirtualDuration:
    """`value` milliseconds of virtual time."""
    return VirtualDuration(_scaled(value, 10**9))


def us(value: float) -> VirtualDuration:
    """`value` microseconds of virtual time."""
    return VirtualDuration(_scaled(value, 10**6))


def ns(value: float) -> VirtualDuration:
    """`value` nanoseconds of virtual time."""
    return VirtualDuration(_scaled(value, 10**3))


def ps(value: int) -> VirtualDuration:
    """`value` picoseconds of virtual time."""
    return VirtualDuration(value)


def Hz(value: float) -> Frequency:
    """`value` hertz."""
    return Frequency(_scaled(value, 1))


def kHz(value: float) -> Frequency:
    """`value` kilohertz."""
    return Frequency(_scaled(value, 10**3))


def MHz(value: float) -> Frequency:
    """`value` megahertz."""
    return Frequency(_scaled(value, 10**6))

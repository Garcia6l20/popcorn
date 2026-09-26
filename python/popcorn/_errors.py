"""The popcorn error hierarchy. Every native error reaches Python as one of these classes."""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING

from popcorn._enums import FaultKind, StopReason

if TYPE_CHECKING:
    from popcorn._stop import Stop


class PopcornError(Exception):
    """Base of every error popcorn raises."""


class ConfigError(PopcornError):
    """A configuration, description or snapshot value is wrong.

    Attributes:
        file: File or source the value came from.
        line: Line in that file, 0 when unknown.
        key: Dotted key of the value.
        expected: What was expected instead.
    """

    def __init__(
        self, message: str, *, file: str = "", line: int = 0, key: str = "", expected: str = ""
    ) -> None:
        super().__init__(message)
        self.file = file
        self.line = line
        self.key = key
        self.expected = expected


class ImageError(PopcornError):
    """An image cannot be read or does not fit the machine's memory.

    Attributes:
        segment: Segment or section concerned.
        address: Guest address concerned, 0 when none.
        region: Region the segment was checked against, empty when none.
    """

    def __init__(
        self, message: str, *, segment: str = "", address: int = 0, region: str = ""
    ) -> None:
        super().__init__(message)
        self.segment = segment
        self.address = address
        self.region = region


class SymbolError(PopcornError):
    """No symbol, or several, has the name.

    Attributes:
        name: The name looked up.
        close_matches: Names close to it, or every match when ambiguous.
    """

    def __init__(self, message: str, *, name: str = "", close_matches: Sequence[str] = ()) -> None:
        super().__init__(message)
        self.name = name
        self.close_matches = tuple(close_matches)


class EmulationError(PopcornError):
    """The emulation cannot continue.

    Attributes:
        reason: Stop reason that ended it, None when not a stop.
        pc: Address of the instruction, None when unknown.
        symbol: Function containing pc, empty when unknown.
        stop: The stop, when the error reports one, such as an unexpected stop of `running()`.
    """

    def __init__(
        self,
        message: str,
        *,
        reason: StopReason | None = None,
        pc: int | None = None,
        symbol: str = "",
        stop: Stop | None = None,
    ) -> None:
        super().__init__(message)
        self.reason = reason
        self.pc = pc
        self.symbol = symbol
        self.stop = stop


class FaultError(EmulationError):
    """A processor fault stopped the emulation.

    Attributes:
        kind: Class of the fault.
        address: Faulting data or instruction address, 0 when the fault has none.
        cfsr: Configurable fault status register, raw.
        hfsr: HardFault status register, raw.
    """

    def __init__(
        self,
        message: str,
        *,
        kind: FaultKind,
        address: int = 0,
        cfsr: int = 0,
        hfsr: int = 0,
        pc: int | None = None,
        symbol: str = "",
        stop: Stop | None = None,
    ) -> None:
        super().__init__(message, reason=StopReason.FAULT, pc=pc, symbol=symbol, stop=stop)
        self.kind = kind
        self.address = address
        self.cfsr = cfsr
        self.hfsr = hfsr


class BudgetExceeded(EmulationError):
    """An instruction budget ran out.

    Attributes:
        budget: The budget, in instructions.
    """

    def __init__(
        self,
        message: str,
        *,
        budget: int = 0,
        pc: int | None = None,
        symbol: str = "",
        stop: Stop | None = None,
    ) -> None:
        super().__init__(message, reason=StopReason.BUDGET, pc=pc, symbol=symbol, stop=stop)
        self.budget = budget


class ModelError(PopcornError):
    """A peripheral model failed, or an unmodelled register was accessed under strict rules.

    Attributes:
        peripheral: Peripheral instance.
        register: Register name, empty when unknown.
        offset: Offset of the access in the peripheral.
    """

    def __init__(
        self, message: str, *, peripheral: str = "", register: str = "", offset: int = 0
    ) -> None:
        super().__init__(message)
        self.peripheral = peripheral
        self.register = register
        self.offset = offset


class MachineRunning(PopcornError):
    """The machine is running, state access is refused outside its handlers."""


class MachineBusy(PopcornError):
    """The machine already runs a slice or a call."""

"""The popcorn error hierarchy. Every native error reaches Python as one of these classes."""

from __future__ import annotations

from collections.abc import Sequence
from typing import TYPE_CHECKING

from popcorn._enums import FaultKind, StopReason

if TYPE_CHECKING:
    from pathlib import Path

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


class MissingInput(ConfigError):
    """A file or directory a machine description needs does not exist: an image, an SVD, a
    description, or the ST install a part's description is built from.

    A malformed file, an unknown part in an existing install or an image that does not fit
    raise another error. `MachineDescription.missing` makes the pytest plugin skip on this one.

    Attributes:
        path: The path that does not exist.
    """

    def __init__(self, message: str, *, path: Path, key: str = "") -> None:
        super().__init__(message, key=key, expected=f"an existing {path}")
        self._missing(path)

    def _missing(self, path: Path) -> None:
        self.path = path


def absent(path: Path) -> bool:
    """Whether `path` does not exist, its parent directory included. False when it exists or
    cannot be checked, such as under a directory without permission: its reader reports that."""
    try:
        path.stat()
    except FileNotFoundError, NotADirectoryError:
        return True
    except OSError:
        return False
    return False


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
        address: The guest address a refused memory access started at, None otherwise.
    """

    def __init__(
        self,
        message: str,
        *,
        reason: StopReason | None = None,
        pc: int | None = None,
        symbol: str = "",
        stop: Stop | None = None,
        address: int | None = None,
    ) -> None:
        super().__init__(message)
        self.reason = reason
        self.pc = pc
        self.symbol = symbol
        self.stop = stop
        self.address = address


class BusAccessError(EmulationError):
    """A host memory view access the bus refused, through `m.mem`, `m.mem.secure` or
    `m.mem.nonsecure`.

    Attributes:
        address: The guest address the refused access started at.
        denied: True when the bus denied the access, such as a Non-secure view access to a
            Secure address, False when nothing is mapped there.
    """

    def __init__(self, message: str, *, address: int, denied: bool) -> None:
        super().__init__(message, address=address)
        self.denied = denied


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


class UsbError(EmulationError):
    """A USB transfer the virtual host ran did not complete.

    Attributes:
        status: How a request ended: `stalled`, `timed_out`, `error`, `overflow` or
            `cancelled`. From `UsbHost.wait_configured`, the enumeration step that failed, such
            as `get_configuration`, or else the device state, such as `DETACHED`. From
            `CdcStream.wait_ready` and `HidStream.wait_ready`, `unbound` or `not_ready`.
    """

    def __init__(self, message: str, *, status: str = "") -> None:
        super().__init__(message)
        self.status = status


class UsbStall(UsbError):
    """The device answered a USB request with STALL."""


class UsbTimeout(UsbError):
    """A USB request, or the enumeration, did not end in time."""


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
    """The machine is busy, a goal active or a free run started: sync access is refused outside
    its handlers. `snapshot`, `restore`, `remove_hooks` and `Ring.clear` refuse handlers too.

    Also raised at once by a sync access from a callback of another machine when its wait
    would close a cycle: the machine's holder waits, directly or through other holders, for a
    machine the caller holds. A wait the user writes in such a callback, on an Event or a
    lock, is not seen: the access then waits until the holder lets go or Ctrl-C."""


class MachineBusy(PopcornError):
    """A run, step, call, wait or free run is already active on the machine."""

"""Stops: why and where a run ended."""

from __future__ import annotations

from dataclasses import dataclass

from popcorn import _core
from popcorn._enums import FaultKind, StopReason
from popcorn._image import Location, location_of
from popcorn.units import VirtualTime


@dataclass(frozen=True, slots=True)
class Fault:
    """A decoded processor fault.

    Attributes:
        kind: Class of the fault.
        address: Faulting data or instruction address, 0 when the fault has none.
        cfsr: Configurable fault status register, raw.
        hfsr: HardFault status register, raw.
    """

    kind: FaultKind
    address: int
    cfsr: int
    hfsr: int


@dataclass(frozen=True, slots=True)
class Stop:
    """Why and where a run, step or wait ended.

    Attributes:
        reason: Why it stopped.
        pc: Address of the next instruction to execute.
        location: Symbol, offset, file and line of pc.
        instructions: Instructions executed since reset.
        time: Virtual time since reset.
        fault: The fault, set exactly when reason is FAULT.
        detail: What caused the stop beyond its reason, such as the loop of a SPIN.
    """

    reason: StopReason
    pc: int
    location: Location
    instructions: int
    time: VirtualTime
    fault: Fault | None = None
    detail: str = ""

    def __str__(self) -> str:
        text = f"{self.reason.name} at {self.location}"
        if self.detail:
            text += f": {self.detail}"
        return text


def stop_of(native: _core.StopInfo) -> Stop:
    """Python copy of a native stop."""
    fault = None
    if native.fault is not None:
        fault = Fault(
            FaultKind(native.fault.kind.value),
            native.fault.fault_address,
            native.fault.status,
            native.fault.detail,
        )
    return Stop(
        reason=StopReason(native.reason.value),
        pc=native.where.pc,
        location=location_of(native.where),
        instructions=native.instructions,
        time=VirtualTime(native.time_ps),
        fault=fault,
        detail=native.detail,
    )

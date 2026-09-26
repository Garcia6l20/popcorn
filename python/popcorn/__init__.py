"""popcorn: MCU emulation for firmware tests, driven from asyncio."""

from popcorn import coverage, trace, units
from popcorn._core import version
from popcorn._description import (
    MachineDescription,
    description_digest,
    load_description,
    parse_description,
)
from popcorn._enums import FaultKind, FloatAbi, ResetKind, StopReason
from popcorn._errors import (
    BudgetExceeded,
    ConfigError,
    EmulationError,
    FaultError,
    ImageError,
    MachineBusy,
    MachineRunning,
    ModelError,
    PopcornError,
    SymbolError,
)
from popcorn._image import Image, Location, Segment, Symbol
from popcorn._machine import Event, Machine, Reached
from popcorn._stop import Fault, Stop

__all__ = [
    "BudgetExceeded",
    "ConfigError",
    "EmulationError",
    "Event",
    "Fault",
    "FaultError",
    "FaultKind",
    "FloatAbi",
    "Image",
    "ImageError",
    "Location",
    "Machine",
    "MachineBusy",
    "MachineDescription",
    "MachineRunning",
    "ModelError",
    "PopcornError",
    "Reached",
    "ResetKind",
    "Segment",
    "Stop",
    "StopReason",
    "Symbol",
    "SymbolError",
    "coverage",
    "description_digest",
    "load_description",
    "parse_description",
    "trace",
    "units",
    "version",
]

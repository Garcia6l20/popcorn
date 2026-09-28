"""popcorn: MCU emulation for firmware tests, driven from asyncio."""

from popcorn import arm, board, coverage, model, report, trace, units
from popcorn._arena import Arena, ArenaBlock
from popcorn._call import F32, I64, CallResult, Stack, f32, i64
from popcorn._core import version
from popcorn._description import (
    MachineDescription,
    description_digest,
    load_description,
    parse_description,
)
from popcorn._dwarf_views import Var
from popcorn._enums import FaultKind, FloatAbi, ResetKind, StopReason, ValueKind
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
from popcorn._hooks import (
    Initiator,
    Intercept,
    LogEntry,
    Return,
    Returned,
    Watch,
    WatchAction,
    WatchHit,
    WatchKind,
)
from popcorn._image import Image, Location, Segment, Symbol
from popcorn._machine import Call, Event, Machine, Reached
from popcorn._observe import MachineCoverage, Ring, TraceRecording
from popcorn._project import BoardEntry, Device, Project, TomlValue
from popcorn._snapshot import Snapshot
from popcorn._stop import Fault, Stop
from popcorn._views import (
    Memory,
    Peripherals,
    PeripheralView,
    RegisterBank,
    RegisterView,
    ScalarView,
)

__all__ = [
    "F32",
    "I64",
    "Arena",
    "ArenaBlock",
    "BoardEntry",
    "BudgetExceeded",
    "Call",
    "CallResult",
    "ConfigError",
    "Device",
    "EmulationError",
    "Event",
    "Fault",
    "FaultError",
    "FaultKind",
    "FloatAbi",
    "Image",
    "ImageError",
    "Initiator",
    "Intercept",
    "Location",
    "LogEntry",
    "Machine",
    "MachineBusy",
    "MachineCoverage",
    "MachineDescription",
    "MachineRunning",
    "Memory",
    "ModelError",
    "PeripheralView",
    "Peripherals",
    "PopcornError",
    "Project",
    "Reached",
    "RegisterBank",
    "RegisterView",
    "ResetKind",
    "Return",
    "Returned",
    "Ring",
    "ScalarView",
    "Segment",
    "Snapshot",
    "Stack",
    "Stop",
    "StopReason",
    "Symbol",
    "SymbolError",
    "TomlValue",
    "TraceRecording",
    "ValueKind",
    "Var",
    "Watch",
    "WatchAction",
    "WatchHit",
    "WatchKind",
    "arm",
    "board",
    "coverage",
    "description_digest",
    "f32",
    "i64",
    "load_description",
    "model",
    "parse_description",
    "report",
    "trace",
    "units",
    "version",
]

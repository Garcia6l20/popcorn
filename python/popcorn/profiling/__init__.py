"""Firmware profiles: `Machine.profile` counts natively while its block runs, then reports
instructions per function, sleeps, exceptions, register accesses and loop runs by name.

    async def main() -> None:
        with m.profile("hotspots", "sleep") as profile:
            await m.run(for_=ms(100))
        print(profile.report().text())

    asyncio.run(main())
"""

from popcorn.profiling._boot import Milestone
from popcorn.profiling._diff import ReportDiff, diff
from popcorn.profiling._export import SCHEMA
from popcorn.profiling._report import MIN_PASSES, NOTES, Profile, Report
from popcorn.profiling._tables import (
    ALL,
    BURST_WINDOW_PS,
    Analysis,
    ClockSpan,
    ExceptionRow,
    FrameKind,
    FrameRow,
    FunctionRow,
    Hotspots,
    Irq,
    LoopRow,
    Loops,
    Peripherals,
    RegisterRow,
    Sleep,
    WakeRow,
    selected,
)
from popcorn.profiling._watchdog import Watchdog, WatchdogGap

__all__ = [
    "ALL",
    "BURST_WINDOW_PS",
    "MIN_PASSES",
    "NOTES",
    "SCHEMA",
    "Analysis",
    "ClockSpan",
    "ExceptionRow",
    "FrameKind",
    "FrameRow",
    "FunctionRow",
    "Hotspots",
    "Irq",
    "LoopRow",
    "Loops",
    "Milestone",
    "Peripherals",
    "Profile",
    "RegisterRow",
    "Report",
    "ReportDiff",
    "Sleep",
    "WakeRow",
    "Watchdog",
    "WatchdogGap",
    "diff",
    "selected",
]

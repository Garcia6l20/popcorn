"""Firmware profiles: `Machine.profile` counts natively while its block runs, then reports
instructions per function, sleeps, exceptions, register accesses and loop runs by name.

    async def main() -> None:
        with m.profile("hotspots", "sleep") as profile:
            await m.run(for_=ms(100))
        print(profile.report().text())

    asyncio.run(main())
"""

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

__all__ = [
    "ALL",
    "BURST_WINDOW_PS",
    "MIN_PASSES",
    "NOTES",
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
    "Peripherals",
    "Profile",
    "RegisterRow",
    "Report",
    "Sleep",
    "WakeRow",
    "selected",
]

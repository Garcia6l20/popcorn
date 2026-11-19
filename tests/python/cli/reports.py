from pathlib import Path

from popcorn.profiling import (
    Analysis,
    ClockSpan,
    FrameKind,
    FrameRow,
    FunctionRow,
    Hotspots,
    Report,
)

FOLDED = ["main 10", "main;a:b 5", "main;[SysTick] 2"]


def synthetic(*, hotspots: bool = True, main_self: int = 10) -> Report:
    frames = (
        FrameRow(-1, FrameKind.ROOT, 0, "", 1, 0, main_self + 7),
        FrameRow(0, FrameKind.ENTRY, 0x100, "main", 1, main_self, main_self + 7),
        FrameRow(1, FrameKind.CALL, 0x200, "a;b", 1, 5, 5),
        FrameRow(1, FrameKind.EXCEPTION, 15, "SysTick", 1, 2, 2),
    )
    rows = (
        FunctionRow("main", 0x100, main_self, main_self + 7, 1),
        FunctionRow("a;b", 0x200, 5, 5, 1),
    )
    return Report(
        analyses=(Analysis.HOTSPOTS,) if hotspots else (),
        start=0,
        from_reset=True,
        elapsed_ticks=main_self + 7,
        elapsed_ps=(main_self + 7) * 1000,
        executed=main_self + 7,
        cuts=0,
        clocks=(ClockSpan(0, 0, 1_000_000_000, main_self + 7, (main_self + 7) * 1000),),
        hotspots=Hotspots(rows, frames, 1, 0, 0) if hotspots else None,
        sleep=None,
        irq=None,
        peripherals=None,
        loops=None,
        watchdog=None,
        timeline=None,
    )


def write(path: Path, report: Report) -> Path:
    path.write_text(report.to_json(), encoding="utf-8")
    return path

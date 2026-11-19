import dataclasses
from pathlib import Path

from popcorn.profiling import (
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
    Milestone,
    Peripherals,
    RegisterRow,
    Report,
    Sleep,
    WakeRow,
    Watchdog,
    WatchdogGap,
)

FOLDED = ["main 10", "main;a:b 5", "main;[SysTick] 2"]


def synthetic(*, hotspots: bool = True, main_self: int = 10, exception_self: int = 2) -> Report:
    total = main_self + 5 + exception_self
    frames = (
        FrameRow(-1, FrameKind.ROOT, 0, "", 1, 0, total),
        FrameRow(0, FrameKind.ENTRY, 0x100, "main", 1, main_self, total),
        FrameRow(1, FrameKind.CALL, 0x200, "a;b", 1, 5, 5),
        FrameRow(1, FrameKind.EXCEPTION, 15, "SysTick", 1, exception_self, exception_self),
    )
    rows = (
        FunctionRow("main", 0x100, main_self, total, 1),
        FunctionRow("a;b", 0x200, 5, 5, 1),
        FunctionRow("SysTick", 15, exception_self, exception_self, 1),
    )
    return Report(
        analyses=(Analysis.HOTSPOTS,) if hotspots else (),
        start=0,
        from_reset=True,
        elapsed_ticks=total,
        elapsed_ps=total * 1000,
        executed=total,
        cuts=0,
        clocks=(ClockSpan(0, 0, 1_000_000_000, total, total * 1000),),
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


TREE = (
    (-1, FrameKind.ROOT, ""),
    (0, FrameKind.ENTRY, "taskA"),
    (0, FrameKind.ENTRY, "taskB"),
    (1, FrameKind.CALL, "a1"),
    (2, FrameKind.CALL, "b1"),
    (1, FrameKind.CALL, "a;2"),
    (2, FrameKind.CALL, "b2"),
    (3, FrameKind.EXCEPTION, "SysTick"),
    (7, FrameKind.ENTRY, "handler"),
    (5, FrameKind.ENTRY, "after_return"),
    (6, FrameKind.CALL, "idle"),
)
TREE_SELF = (0, 3, 4, 5, 6, 7, 8, 2, 9, 1, 0)


def tree_frames(selfs: tuple[int, ...] = TREE_SELF) -> tuple[FrameRow, ...]:
    inclusive = list(selfs)
    for index in range(len(TREE) - 1, 0, -1):
        inclusive[TREE[index][0]] += inclusive[index]
    return tuple(
        FrameRow(parent, kind, index, name, 1, selfs[index], inclusive[index])
        for index, (parent, kind, name) in enumerate(TREE)
    )


def tree_report(frames: tuple[FrameRow, ...] | None = None) -> Report:
    base = synthetic()
    chosen = tree_frames() if frames is None else frames
    assert base.hotspots is not None
    total = sum(frame.self for frame in chosen)
    merged: dict[str, FunctionRow] = {}
    for frame in chosen[1:]:
        row = merged.get(frame.name)
        merged[frame.name] = FunctionRow(
            frame.name,
            frame.key,
            frame.self + (row.self if row else 0),
            frame.inclusive + (row.inclusive if row else 0),
            frame.entries + (row.calls if row else 0),
        )
    functions = tuple(sorted(merged.values(), key=lambda row: -row.self))
    return dataclasses.replace(
        base,
        elapsed_ticks=total,
        elapsed_ps=total * 1000,
        executed=total,
        clocks=(ClockSpan(0, 0, 1_000_000_000, total, total * 1000),),
        hotspots=dataclasses.replace(base.hotspots, functions=functions, frames=chosen),
    )


def full_report(extra: int = 0) -> Report:
    base = synthetic(main_self=10 + extra, exception_self=2 + extra)
    sleep_ps = 5000 + extra * 1000
    ticks = base.executed + sleep_ps // 1000
    elapsed_ps = ticks * 1000
    exception_ps = (2 + extra) * 1000
    exception = ExceptionRow(
        15,
        "SysTick",
        1,
        1 / (elapsed_ps / 1e9),
        exception_ps,
        exception_ps,
        exception_ps,
        1,
        0,
        0,
        (),
        0,
        0,
    )
    register = RegisterRow("TIM2.CNT", 0x4000_0024, 100 + extra, 5, 1, 0)
    loop_ps = 7000 + extra * 1000
    return dataclasses.replace(
        base,
        analyses=tuple(Analysis),
        elapsed_ticks=ticks,
        elapsed_ps=elapsed_ps,
        clocks=(ClockSpan(0, 0, 1_000_000_000, ticks, elapsed_ps),),
        sleep=Sleep(
            2,
            sleep_ps,
            sleep_ps,
            sleep_ps / elapsed_ps,
            {"wfi": 2},
            0,
            (WakeRow("SysTick", 2, sleep_ps),),
            0,
        ),
        irq=Irq((exception,), 1000, 0),
        peripherals=Peripherals((register,), (RegisterRow("DMA1.CNDTR", 0x4002_0004, 3, 0, 0, 0),)),
        loops=Loops(
            (LoopRow(0x100, "main+4", 0x104, 2, 50, loop_ps, loop_ps / elapsed_ps, {"irq": 2}),), 8
        ),
        watchdog=Watchdog(
            3 + extra,
            1,
            2_000_000,
            500_000,
            0x110,
            (WatchdogGap(0, 3_000_000, 2_000_000, 0x110),),
            4,
            None,
        ),
        timeline=(Milestone("main", 0x100, 1000 + extra), Milestone("late", 0x200, None)),
    )

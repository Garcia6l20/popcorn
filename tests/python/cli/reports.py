import dataclasses
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

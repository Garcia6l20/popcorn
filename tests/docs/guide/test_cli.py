from pathlib import Path

import pytest
from click.testing import CliRunner
from popcorn.cli import cli
from popcorn.coverage import BlockHit, CodeRange, Coverage, CoverageGroup, EdgeHit, ImageRef
from popcorn.profiling import (
    Analysis,
    ClockSpan,
    ExceptionRow,
    FrameKind,
    FrameRow,
    FunctionRow,
    Hotspots,
    Irq,
    Report,
    Sleep,
    WakeRow,
)

SNIPPETS = Path(__file__).parent / "cli"
LINES_ELF = Path(__file__).parents[2] / "firmware/obs/prebuilt/lines.elf"
HZ = 16_000_000
PS_PER_TICK = 62_500
CHECK = "prvCheckTasksWaitingTermination"


def report(idle: int, check: int, flag: int, status: int, systick: int, sleep_ticks: int) -> Report:
    executed = idle + check + flag + status + systick
    ticks = executed + sleep_ticks
    elapsed_ps = ticks * PS_PER_TICK
    sleep_ps = sleep_ticks * PS_PER_TICK
    systick_ps = systick * PS_PER_TICK
    frames = (
        FrameRow(-1, FrameKind.ROOT, 0, "", 1, 0, executed),
        FrameRow(0, FrameKind.ENTRY, 0x0800_1000, "prvIdleTask", 1, idle, idle + check),
        FrameRow(
            1, FrameKind.CALL, 0x0800_2000, "prvCheckTasksWaitingTermination", 1, check, check
        ),
        FrameRow(0, FrameKind.ENTRY, 0x0800_3000, "flag_task", 1, flag, flag),
        FrameRow(0, FrameKind.ENTRY, 0x0800_4000, "status_task", 1, status, status),
        FrameRow(0, FrameKind.EXCEPTION, 15, "SysTick", 1, systick, systick),
    )
    functions = tuple(
        FunctionRow(f.name, f.key, f.self, f.inclusive, f.entries) for f in frames[1:]
    )
    exception = ExceptionRow(
        15,
        "SysTick",
        1000,
        1000 / (elapsed_ps / 1e9),
        systick_ps,
        systick_ps,
        systick_ps // 1000,
        1,
        0,
        0,
        (),
        0,
        0,
    )
    return Report(
        analyses=(Analysis.HOTSPOTS, Analysis.SLEEP, Analysis.IRQ),
        start=0,
        from_reset=True,
        elapsed_ticks=ticks,
        elapsed_ps=elapsed_ps,
        executed=executed,
        cuts=0,
        clocks=(ClockSpan(0, 0, HZ, ticks, elapsed_ps),),
        hotspots=Hotspots(functions, frames, 1000, 0, 0),
        sleep=Sleep(
            1000 if sleep_ticks else 0,
            sleep_ps,
            sleep_ps,
            sleep_ps / elapsed_ps,
            {"wfi": 1000} if sleep_ticks else {},
            0,
            (WakeRow("SysTick", 1000, sleep_ps),) if sleep_ticks else (),
            0,
        ),
        irq=Irq((exception,), 1000, 0),
        peripherals=None,
        loops=None,
        watchdog=None,
        timeline=None,
    )


SPIN = report(5_429_491, 9_501_610, 665_198, 286_277, 113_000, 0)
WFI = report(52_000, 31_000, 665_198, 286_277, 113_000, 14_300_000)


def coverage(hits: int) -> Coverage:
    return Coverage.from_parts(
        [
            CoverageGroup(
                images=(ImageRef.from_elf(LINES_ELF),),
                ranges=(CodeRange("clamp", 0x0800_0008, 0x10),),
                blocks=(
                    BlockHit(0x0800_0008, 4, hits),
                    BlockHit(0x0800_0014, 4, 1),
                    BlockHit(0x2000_0000, 2, 1),
                ),
                edges=(EdgeHit(0x0800_0008, 0x0800_0014, 1),),
            )
        ]
    )


@pytest.fixture
def shell(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.chdir(tmp_path)
    (tmp_path / "spin.json").write_text(SPIN.to_json())
    (tmp_path / "wfi.json").write_text(WFI.to_json())
    (tmp_path / "reports").mkdir()
    (tmp_path / "reports" / "spin.json").write_text(SPIN.to_json())
    (tmp_path / "reports" / "wfi.json").write_text(WFI.to_json())
    coverage(2).save(tmp_path / "gw0.pcov")
    coverage(3).save(tmp_path / "gw1.pcov")
    return tmp_path


def transcript(*args: str) -> str:
    result = CliRunner().invoke(cli, list(args))
    assert result.exit_code == 0, result.output
    return f"$ popcorn {' '.join(args)}\n{result.output}"


def check(name: str, *args: str, lines: int | None = None) -> None:
    text = transcript(*args)
    if lines is not None:
        text = "".join(text.splitlines(keepends=True)[:lines])
    assert text == (SNIPPETS / name).read_text()


@pytest.mark.usefixtures("shell")
def test_profile_show() -> None:
    check("show.txt", "profile", "show", "spin.json", lines=21)


@pytest.mark.usefixtures("shell")
def test_profile_folded() -> None:
    check("folded.txt", "profile", "folded", "spin.json")


def test_profile_chrome(shell: Path) -> None:
    check("chrome.txt", "profile", "chrome", "spin.json", "-o", "spin.cpuprofile")
    assert (shell / "spin.cpuprofile").stat().st_size > 0


@pytest.mark.usefixtures("shell")
def test_profile_diff() -> None:
    check(
        "diff.txt", "profile", "diff", "spin.json", "wfi.json", "--table", "functions", "--top", "3"
    )


@pytest.mark.usefixtures("shell")
def test_profile_summary() -> None:
    check("summary.txt", "profile", "summary", "reports", "--functions", "2")


def test_coverage_merge(shell: Path) -> None:
    check("merge.txt", "coverage", "merge", "gw0.pcov", "gw1.pcov", "-o", "all.pcov")
    assert Coverage.load(shell / "all.pcov").groups[0].blocks[0].hits == 5


def test_coverage_export(shell: Path) -> None:
    coverage(5).save(shell / "all.pcov")
    check(
        "export.txt", "coverage", "export", "all.pcov", "--lcov", "all.info", "--drcov", "all.drcov"
    )
    assert (shell / "all.info").read_text().startswith("TN:")
    assert (shell / "all.drcov").stat().st_size > 0


def test_part_explain() -> None:
    check("explain.txt", "part", "explain", "STM32U575QGIx", lines=7)

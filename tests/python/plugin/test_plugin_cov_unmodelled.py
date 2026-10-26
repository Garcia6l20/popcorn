"""`--popcorn-cov-unmodelled`: the option, the sidecar, the term column and the boot cache."""

from __future__ import annotations

from pathlib import Path

import pytest
from popcorn.coverage import unmodelled_path
from popcorn.coverage.report import parse_lcov, parse_unmodelled
from popcorn.pytest._cache import cache_key

from .helpers import ROOT, write_conftest, write_project

pytest_plugins = ["pytester"]

ELF = ROOT / "tests" / "firmware" / "obs" / "prebuilt" / "unmodelled.elf"
NO_XDIST = ("-p", "no:asyncio", "-p", "no:xdist")

TESTS = """
async def test_poll(machine):
    assert (await machine.call("poll_unmodelled", 3)).value == 0

async def test_stat(machine):
    await machine.call("touch_stat", 5)
"""


def project(pytester: pytest.Pytester, tests: str = TESTS) -> None:
    write_conftest(pytester)
    write_project(pytester, "boot_done", elf=ELF)
    (pytester.path / "test_unmodelled.py").write_text(tests)


def run(pytester: pytest.Pytester, mode: str | None, *args: str) -> pytest.RunResult:
    given = [] if mode is None else [f"--popcorn-cov-unmodelled={mode}"]
    return pytester.runpytest(*NO_XDIST, "--popcorn-cov", *given, *args)


def hit_lines(path: Path) -> dict[int, int]:
    (file,) = parse_lcov(path.read_text()).files
    return dict(file.lines)


def test_mark_writes_the_sidecar_and_a_term_column(pytester: pytest.Pytester) -> None:
    project(pytester)
    target = pytester.path / "out" / "cov.info"
    result = run(
        pytester, "mark", f"--popcorn-cov-report=lcov:{target}", "--popcorn-cov-report=term"
    )
    result.assert_outcomes(passed=2)
    result.stdout.fnmatch_lines(["*Unmodelled", "*TOTAL*"])
    uses, unplaced = parse_unmodelled(unmodelled_path(target).read_text())
    lines = hit_lines(target)
    assert unplaced == 0
    assert len(uses) == 4
    assert all(lines[line] > 0 for _, line in uses)
    assert all(path.endswith("unmodelled.c") for path, _ in uses)


def test_hit_is_the_default_and_leaves_the_lcov_alone(pytester: pytest.Pytester) -> None:
    project(pytester)
    run(pytester, None, "--popcorn-cov-report=lcov:out/default.info").assert_outcomes(passed=2)
    run(pytester, "hit", "--popcorn-cov-report=lcov:out/hit.info").assert_outcomes(passed=2)
    default = pytester.path / "out" / "default.info"
    assert default.read_bytes() == (pytester.path / "out" / "hit.info").read_bytes()
    assert not unmodelled_path(default).exists()


def test_miss_zeroes_the_lines_and_rewrites_the_lcov(pytester: pytest.Pytester) -> None:
    project(pytester)
    run(pytester, "mark", "--popcorn-cov-report=lcov:out/mark.info").assert_outcomes(passed=2)
    run(pytester, "miss", "--popcorn-cov-report=lcov:out/miss.info").assert_outcomes(passed=2)
    uses, _ = parse_unmodelled(unmodelled_path(pytester.path / "out" / "mark.info").read_text())
    marked = hit_lines(pytester.path / "out" / "mark.info")
    missed = hit_lines(pytester.path / "out" / "miss.info")
    zeroed = {line for _, line in uses}
    assert zeroed and all(missed[line] == 0 for line in zeroed)
    assert {line: count for line, count in missed.items() if line not in zeroed} == {
        line: count for line, count in marked.items() if line not in zeroed
    }
    assert unmodelled_path(pytester.path / "out" / "miss.info").is_file()


def test_the_mode_is_checked_at_startup(pytester: pytest.Pytester) -> None:
    project(pytester)
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", "--popcorn-cov-unmodelled=bogus")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    needs = pytester.runpytest(*NO_XDIST, "--popcorn-cov-unmodelled=mark")
    assert needs.ret == pytest.ExitCode.USAGE_ERROR
    needs.stderr.fnmatch_lines(["*--popcorn-cov-unmodelled needs --popcorn-cov*"])
    pytester.makeini("[pytest]\npopcorn_cov_unmodelled = bogus\n")
    ini = pytester.runpytest(*NO_XDIST, "--popcorn-cov")
    assert ini.ret == pytest.ExitCode.USAGE_ERROR
    ini.stderr.fnmatch_lines(["*popcorn_cov_unmodelled must be one of mark, miss, hit*"])


def test_the_ini_option_sets_the_mode(pytester: pytest.Pytester) -> None:
    project(pytester)
    pytester.makeini("[pytest]\npopcorn_cov_unmodelled = mark\n")
    run(pytester, None, "--popcorn-cov-report=lcov:out/ini.info").assert_outcomes(passed=2)
    assert unmodelled_path(pytester.path / "out" / "ini.info").is_file()


def test_every_machine_records_in_mark_mode_across_the_boot_cache(
    pytester: pytest.Pytester,
) -> None:
    project(pytester)

    def sums(name: str, mode: str | None) -> tuple[str, dict[int, int]]:
        target = pytester.path / "out" / f"{name}.info"
        result = run(pytester, mode, f"--popcorn-cov-report=lcov:{target}")
        result.assert_outcomes(passed=2)
        boots = next(line for line in result.outlines if "key(s)" in line)
        if mode != "mark":
            return boots, {}
        uses, _ = parse_unmodelled(unmodelled_path(target).read_text())
        return boots, {line: sum(u.hits for u in group) for (_, line), group in uses.items()}

    assert sums("a", "hit")[0] == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    first, hits = sums("b", "mark")
    assert first == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert sorted(hits.values()) == [1, 1, 2, 3]
    again, same = sums("c", "mark")
    assert again == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    assert same == hits


def test_the_cache_key_names_the_recording(pytester: pytest.Pytester) -> None:
    import popcorn

    from .helpers import DRIVER_ELF, M33_JSON

    description = popcorn.MachineDescription(
        part="POPCORN-M33-TEST",
        description=M33_JSON,
        images=[popcorn.Elf(DRIVER_ELF)],
    )
    plain = cache_key(description, False)
    assert cache_key(description, False, unmodelled=False) == plain
    assert cache_key(description, False, unmodelled=True) != plain

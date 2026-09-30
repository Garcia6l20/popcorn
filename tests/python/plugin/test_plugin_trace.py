"""`--popcorn-trace-on-fail`: a full trace of each failing test, FR-OBS-4 through pytest."""

from __future__ import annotations

import hashlib
import re

import popcorn
import pytest
from popcorn.trace import Block, read

from .helpers import DRIVER_ELF, write_conftest, write_project

pytest_plugins = ["pytester"]

NO_ASYNCIO = ("-p", "no:asyncio")

TESTS = """
import pytest

async def test_fails(machine):
    await machine.call("alu_mark")
    assert False

def test_passes(machine):
    pass
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def expected_name(nodeid: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_.-]", "_", nodeid)
    return f"{sanitized[:150]}-{hashlib.sha256(nodeid.encode()).hexdigest()[:8]}.pctr"


def alu_mark() -> int:
    return popcorn.Image.elf(DRIVER_ELF).lookup("alu_mark").value & ~1


def test_a_failing_test_keeps_its_trace_in_dir(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    traces = pytester.path / "traces"
    result = run(pytester, f"--popcorn-trace-on-fail={traces}")
    result.assert_outcomes(passed=1, failed=1)
    kept = sorted(traces.iterdir())
    name = expected_name("test_a_failing_test_keeps_its_trace_in_dir.py::test_fails")
    assert [path.name for path in kept] == [name]
    pcs = {record.pc for record in read(kept[0]) if isinstance(record, Block)}
    assert alu_mark() in pcs
    result.stdout.fnmatch_lines(["*- popcorn -*", f"trace: {traces / name}"])


def test_the_bare_flag_uses_the_pytest_cache(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    run(pytester, "--popcorn-trace-on-fail").assert_outcomes(passed=1, failed=1)
    traces = pytester.path / ".pytest_cache" / "d" / "popcorn" / "traces"
    assert [path.suffix for path in traces.iterdir()] == [".pctr"]


def test_without_the_cache_the_rootdir_holds_them(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    result = run(pytester, "-p", "no:cacheprovider", "--popcorn-trace-on-fail")
    result.assert_outcomes(passed=1, failed=1)
    assert len(list((pytester.path / ".popcorn-traces").iterdir())) == 1


def test_parametrized_ids_are_sanitized(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.parametrize("value", ["a/b"])
        def test_x(machine, value):
            assert False
        """)
    traces = pytester.path / "traces"
    run(pytester, f"--popcorn-trace-on-fail={traces}").assert_outcomes(failed=1)
    assert [path.name for path in traces.iterdir()] == [
        expected_name("test_parametrized_ids_are_sanitized.py::test_x[a/b]")
    ]


def test_without_the_option_nothing_is_traced(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1)
    assert not (pytester.path / ".pytest_cache" / "d" / "popcorn").exists()
    result.stdout.no_fnmatch_line("trace: *")


def test_xdist_workers_prefix_their_traces(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    traces = pytester.path / "traces"
    result = pytester.runpytest_subprocess(
        *NO_ASYNCIO, "-p", "xdist", "-n", "2", f"--popcorn-trace-on-fail={traces}"
    )
    result.assert_outcomes(passed=1, failed=1)
    names = [path.name for path in traces.iterdir()]
    assert len(names) == 1
    assert names[0].startswith("gw")
    assert names[0].endswith(expected_name("test_xdist_workers_prefix_their_traces.py::test_fails"))


def test_colliding_ids_keep_two_traces(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.parametrize("value", ["a/b", "a_b"])
        def test_x(machine, value):
            assert False
        """)
    traces = pytester.path / "traces"
    run(pytester, f"--popcorn-trace-on-fail={traces}").assert_outcomes(failed=2)
    assert len(list(traces.iterdir())) == 2


def test_a_long_id_is_shortened(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.mark.parametrize("value", ["x" * 300])
        def test_long(machine, value):
            assert False
        """)
    traces = pytester.path / "traces"
    run(pytester, f"--popcorn-trace-on-fail={traces}").assert_outcomes(failed=1)
    names = [path.name for path in traces.iterdir()]
    assert len(names) == 1
    assert len(names[0]) == 150 + len("-12345678.pctr")


def test_a_trace_that_cannot_start_warns_and_the_test_runs(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        def test_runs(machine):
            assert machine.pc == machine.sym("alu_mark")
        """)
    blocker = pytester.path / "blocker"
    blocker.write_text("a file, not a directory")
    result = run(pytester, f"--popcorn-trace-on-fail={blocker / 'traces'}")
    result.assert_outcomes(passed=1, warnings=1)
    result.stdout.fnmatch_lines(["*popcorn could not trace*"])


def test_a_later_setup_error_keeps_no_trace(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        import pytest

        @pytest.fixture
        def broken():
            raise RuntimeError("setup broke")

        def test_setup(machine, broken):
            pass
        """)
    traces = pytester.path / "traces"
    result = run(pytester, f"--popcorn-trace-on-fail={traces}")
    result.assert_outcomes(errors=1)
    assert list(traces.iterdir()) == []
    result.stdout.no_fnmatch_line("trace: *")


def test_stale_part_files_are_swept(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(TESTS)
    traces = pytester.path / "traces"
    traces.mkdir()
    stale = traces / "old.pctr.part"
    stale.write_text("left by an interrupted session")
    run(pytester, f"--popcorn-trace-on-fail={traces}").assert_outcomes(passed=1, failed=1)
    assert not stale.exists()
    assert [path.suffix for path in traces.iterdir()] == [".pctr"]

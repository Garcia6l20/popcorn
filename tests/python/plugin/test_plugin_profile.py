"""`--popcorn-profile` and the `profile` marker keyword: one profile report per machine and test."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

import pytest
from popcorn.profiling import ALL, Analysis, Report

from .helpers import cov_args, write_conftest, write_project
from .test_plugin_system import PROJECTS, USART_LL

pytest_plugins = ["pytester"]
pytestmark = pytest.mark.timeout(60)

NO_ASYNCIO = ("-p", "no:asyncio")

TESTS = """
import pytest

async def test_calls(machine):
    await machine.call("alu_mark")

async def test_fails(machine):
    await machine.call("alu_mark")
    assert False
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*NO_ASYNCIO, *args)


def profile_name(nodeid: str, label: str = "machine") -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_.-]", "_", nodeid)
    digest = hashlib.sha256(nodeid.encode()).hexdigest()[:8]
    return f"{sanitized[:150]}-{digest}.{re.sub(r'[^A-Za-z0-9_.-]', '_', label)}.json"


def project(pytester: pytest.Pytester, tests: str = TESTS) -> str:
    write_conftest(pytester)
    write_project(pytester)
    path = pytester.makepyfile(tests)
    return path.name


def reports(directory: Path) -> dict[str, Report]:
    return {
        path.name: Report.from_json(path.read_text(encoding="utf-8"))
        for path in sorted(directory.glob("*.json"))
    }


def test_the_option_writes_one_report_per_test_whatever_its_outcome(
    pytester: pytest.Pytester,
) -> None:
    module = project(pytester)
    result = run(pytester, "--popcorn-profile")
    result.assert_outcomes(passed=1, failed=1)
    directory = pytester.path / "popcorn-profile"
    written = reports(directory)
    assert sorted(written) == sorted(
        profile_name(f"{module}::{name}") for name in ("test_calls", "test_fails")
    )
    for report in written.values():
        assert report.analyses == ALL
        assert report.executed > 0
        assert not report.from_reset
        assert report.hotspots is not None
        assert "alu_mark" in {row.name for row in report.hotspots.functions}
    result.stdout.fnmatch_lines([f"popcorn: 2 profile(s) written to {directory}"])
    shown = run(pytester, "--popcorn-profile=hotspots", "-rA")
    shown.stdout.fnmatch_lines(
        [
            "*- Captured popcorn profile machine teardown -*",
            f"{directory / profile_name(f'{module}::test_calls')}",
            "profile from instruction *",
        ]
    )
    assert not list(directory.glob("*.part"))


def test_the_option_names_the_analyses(pytester: pytest.Pytester) -> None:
    project(pytester)
    run(pytester, "--popcorn-profile=sleep, hotspots").assert_outcomes(passed=1, failed=1)
    written = reports(pytester.path / "popcorn-profile")
    assert len(written) == 2
    for report in written.values():
        assert report.analyses == (Analysis.HOTSPOTS, Analysis.SLEEP)
        assert report.irq is None


def test_an_unknown_analysis_is_a_usage_error(pytester: pytest.Pytester) -> None:
    project(pytester)
    result = run(pytester, "--popcorn-profile=hotspots,nope")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*--popcorn-profile: no analysis 'nope'*"])


def test_a_bare_flag_before_a_path_names_the_placement_rule(pytester: pytest.Pytester) -> None:
    module = project(pytester)
    result = run(pytester, "--popcorn-profile", module)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(
        [
            f"*--popcorn-profile: no analysis '{module}', the analyses are *: write"
            " --popcorn-profile=ANALYSES or put the flag last, a bare flag before a path takes"
            " the path as its ANALYSES"
        ]
    )
    run(pytester, module, "--popcorn-profile").assert_outcomes(passed=1, failed=1)
    assert len(reports(pytester.path / "popcorn-profile")) == 2


def test_without_the_option_nothing_is_written(pytester: pytest.Pytester) -> None:
    project(pytester)
    result = run(pytester)
    result.assert_outcomes(passed=1, failed=1)
    assert not (pytester.path / "popcorn-profile").exists()
    assert "profile(s) written" not in result.stdout.str()


def test_the_marker_profiles_its_test_and_false_turns_the_option_off(
    pytester: pytest.Pytester,
) -> None:
    module = project(
        pytester,
        """
        import pytest

        @pytest.mark.popcorn(profile="irq")
        async def test_marked(machine):
            await machine.call("alu_mark")

        @pytest.mark.popcorn(profile=False)
        async def test_off(machine):
            await machine.call("alu_mark")

        async def test_plain(machine):
            await machine.call("alu_mark")
        """,
    )
    run(pytester).assert_outcomes(passed=3)
    written = reports(pytester.path / "popcorn-profile")
    assert list(written) == [profile_name(f"{module}::test_marked")]
    assert written[profile_name(f"{module}::test_marked")].analyses == (Analysis.IRQ,)
    run(pytester, "--popcorn-profile=hotspots").assert_outcomes(passed=3)
    written = reports(pytester.path / "popcorn-profile")
    assert sorted(written) == sorted(
        profile_name(f"{module}::{name}") for name in ("test_marked", "test_plain")
    )
    assert written[profile_name(f"{module}::test_marked")].analyses == (Analysis.IRQ,)
    assert written[profile_name(f"{module}::test_plain")].analyses == (Analysis.HOTSPOTS,)


def test_a_none_profile_keyword_leaves_it_to_the_option(pytester: pytest.Pytester) -> None:
    module = project(
        pytester,
        """
        import pytest

        @pytest.mark.popcorn(profile=None)
        async def test_marked(machine):
            await machine.call("alu_mark")
        """,
    )
    run(pytester).assert_outcomes(passed=1)
    assert not (pytester.path / "popcorn-profile").exists()
    run(pytester, "--popcorn-profile=sleep").assert_outcomes(passed=1)
    written = reports(pytester.path / "popcorn-profile")
    assert written[profile_name(f"{module}::test_marked")].analyses == (Analysis.SLEEP,)


@pytest.mark.parametrize(
    ("keyword", "message"),
    [
        ("3", "profile is a bool, an analysis name or a sequence of them, not 3"),
        ("()", "profile names no analysis: use True for every one"),
        ("('irq', 4)", "profile is a bool, an analysis name or a sequence of them"),
        ("'nope'", "profile: no analysis 'nope'"),
    ],
)
def test_a_bad_profile_keyword_fails_the_test(
    pytester: pytest.Pytester, keyword: str, message: str
) -> None:
    project(
        pytester,
        f"""
        import pytest

        @pytest.mark.popcorn(profile={keyword})
        def test_marked(machine):
            pass
        """,
    )
    result = run(pytester)
    result.assert_outcomes(errors=1)
    result.stdout.fnmatch_lines([f"*@pytest.mark.popcorn {message}*"])
    assert not (pytester.path / "popcorn-profile").exists()


def test_the_ini_option_moves_the_directory(pytester: pytest.Pytester) -> None:
    project(pytester)
    pytester.makeini("[pytest]\npopcorn_profile_dir = out/profiles\n")
    run(pytester, "--popcorn-profile").assert_outcomes(passed=1, failed=1)
    assert len(reports(pytester.path / "out" / "profiles")) == 2
    assert not (pytester.path / "popcorn-profile").exists()


def test_machine_factory_machines_are_profiled_booted_or_not(pytester: pytest.Pytester) -> None:
    module = project(
        pytester,
        """
        async def test_factory(machine_factory, description):
            booted = await machine_factory.booted(description)
            await booted.call("alu_mark")
            cold = machine_factory(description, booted=False)
            cold.reset()
            await cold.run(until="alu_mark", budget=100_000)
        """,
    )
    run(pytester, "--popcorn-profile=hotspots").assert_outcomes(passed=1)
    written = reports(pytester.path / "popcorn-profile")
    nodeid = f"{module}::test_factory"
    first = written[profile_name(nodeid, "machine_factory #1")]
    second = written[profile_name(nodeid, "machine_factory #2")]
    assert len(written) == 2
    assert not first.from_reset
    assert second.from_reset
    assert second.cuts == 1


FAILING_REPORT = """
from pathlib import Path

import popcorn
from popcorn.profiling import Report


def failing_report(monkeypatch):
    def refuse(report):
        raise RuntimeError("no JSON for this report")

    close_loop = popcorn.Machine._close_loop

    def closing(machine):
        with Path("closed.txt").open("a") as closed:
            closed.write(f"{machine.name}\\n")
        close_loop(machine)

    monkeypatch.setattr(Report, "to_json", refuse)
    monkeypatch.setattr(popcorn.Machine, "_close_loop", closing)
"""


def test_a_profile_error_still_closes_every_factory_machine(pytester: pytest.Pytester) -> None:
    project(
        pytester,
        FAILING_REPORT + """

async def test_factory(monkeypatch, machine_factory, description):
    failing_report(monkeypatch)
    first = await machine_factory.booted(description, name="first")
    second = await machine_factory.booted(description, name="second")
    await first.call("alu_mark")
    await second.call("alu_mark")
""",
    )
    result = run(pytester, "--popcorn-profile=sleep")
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(["*RuntimeError: no JSON for this report*"])
    assert (pytester.path / "closed.txt").read_text().split() == ["first", "second"]


def test_a_profile_error_keeps_the_failed_test_trace(pytester: pytest.Pytester) -> None:
    project(
        pytester,
        FAILING_REPORT + """

async def test_fails(monkeypatch, machine):
    failing_report(monkeypatch)
    await machine.call("alu_mark")
    assert False
""",
    )
    result = run(pytester, "--popcorn-profile=sleep", "--popcorn-trace-on-fail=traces")
    result.assert_outcomes(failed=1, errors=1)
    result.stdout.fnmatch_lines(["*RuntimeError: no JSON for this report*"])
    assert len(list((pytester.path / "traces").glob("*.pctr"))) == 1


def test_the_next_test_on_a_fixture_machine_has_no_profile_left(pytester: pytest.Pytester) -> None:
    project(
        pytester,
        """
        import pytest

        @pytest.mark.popcorn(profile="loops")
        async def test_first(machine):
            assert machine._handle.loop_reports() != 0

        async def test_second(machine):
            assert machine._handle.loop_reports() == 0
        """,
    )
    run(pytester, "-p", "no:randomly").assert_outcomes(passed=2)


def test_session_coverage_counts_the_same_with_a_profile(pytester: pytest.Pytester) -> None:
    project(pytester)
    run(pytester, *cov_args("plain.info")).assert_outcomes(passed=1, failed=1)
    run(pytester, *cov_args("profiled.info"), "--popcorn-profile").assert_outcomes(
        passed=1, failed=1
    )
    plain = (pytester.path / "plain.info").read_text()
    assert "DA:" in plain
    assert (pytester.path / "profiled.info").read_text() == plain


def test_the_plugin_imports_no_profiling_module_without_the_option(
    pytester: pytest.Pytester,
) -> None:
    project(pytester)
    pytester.makeconftest((pytester.path / "conftest.py").read_text() + """

def pytest_sessionfinish(session):
    import sys

    Path("modules.txt").write_text(str("popcorn.profiling" in sys.modules))
""")
    pytester.runpytest_subprocess(*NO_ASYNCIO).assert_outcomes(passed=1, failed=1)
    assert (pytester.path / "modules.txt").read_text() == "False"


SYSTEM = """
import popcorn
import popcorn.pytest
from popcorn.units import us

from link_projects import A, B


@popcorn.pytest.system
def system():
    return popcorn.SystemDescription(
        members={"a": A, "b": B},
        links=[popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )
"""


@pytest.fixture(scope="module")
def u575_json() -> Path:
    from popcorn.stm32.description import DescriptionError, cached_description

    try:
        return Path(cached_description("STM32U575QGIx"))
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


def test_system_members_are_not_profiled_and_a_marker_asking_fails(
    pytester: pytest.Pytester, u575_json: Path
) -> None:
    write_conftest(pytester)
    (pytester.path / "link_projects.py").write_text(
        PROJECTS.format(description=str(u575_json), elf=str(USART_LL))
    )
    (pytester.path / "conftest.py").write_text((pytester.path / "conftest.py").read_text() + SYSTEM)
    pytester.makepyfile("""
        import pytest

        def test_linked(system):
            assert set(system.members) == {"a", "b"}

        @pytest.mark.popcorn(profile=True)
        def test_marked(system):
            pass
        """)
    result = run(pytester, "--popcorn-profile")
    result.assert_outcomes(passed=1, errors=1)
    result.stdout.fnmatch_lines(
        ["*@pytest.mark.popcorn profile covers machine fixtures and machine_factory machines*"]
    )
    assert not (pytester.path / "popcorn-profile").exists()


def test_xdist_workers_name_the_files_by_node_id_alone(pytester: pytest.Pytester) -> None:
    module = project(pytester)
    result = run(pytester, "--popcorn-profile=sleep", "-n", "2", "-p", "xdist")
    result.assert_outcomes(passed=1, failed=1)
    assert sorted(reports(pytester.path / "popcorn-profile")) == sorted(
        profile_name(f"{module}::{name}") for name in ("test_calls", "test_fails")
    )
    result.stdout.fnmatch_lines(
        [f"popcorn: 2 profile(s) written to {pytester.path / 'popcorn-profile'}"]
    )

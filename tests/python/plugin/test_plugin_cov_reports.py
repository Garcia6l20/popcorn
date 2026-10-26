"""Coverage options: the flag, the report specs, the ini options and their precedence."""

from __future__ import annotations

import shutil
import textwrap
import xml.etree.ElementTree as ElementTree
from decimal import Decimal
from pathlib import Path

import pytest
from popcorn.coverage.report import display_name, parse_lcov, truncated_percent

from .coverage_suite import write_suite
from .helpers import ROOT, cov_args, write_conftest, write_project
from .test_plugin_genhtml import (
    CUT_ELF,
    CUT_TESTS,
    SHARED_EXIT_ELF,
    SHARED_EXIT_TESTS,
    session,
)

pytest_plugins = ["pytester"]

CUT_SOURCE = ROOT / "tests" / "firmware" / "obs" / "cut.c"

NO_XDIST = ("-p", "no:asyncio", "-p", "no:xdist")

SNAPSHOT_PRE_MERGE = """
from popcorn.pytest import _coverage

merge = _coverage.merge_session


def spy(directory, lcov, **kwargs):
    merge(directory, "pre.info", **kwargs)
    return merge(directory, lcov, **kwargs)


_coverage.merge_session = spy
"""

SETTINGS_TEST = """
from pathlib import Path

from popcorn.pytest._options import SETTINGS


def test_settings(request):
    settings = request.config.stash[SETTINGS]
{checks}
"""


def settings_test(checks: str) -> str:
    return SETTINGS_TEST.format(checks=textwrap.indent(textwrap.dedent(checks).strip(), "    "))


def check_settings(pytester: pytest.Pytester, checks: str, *args: str) -> pytest.RunResult:
    write_conftest(pytester)
    pytester.makepyfile(settings_test(checks))
    return pytester.runpytest("-p", "no:asyncio", *args)


def test_lcov_report_matches_the_merged_session_bytes(pytester: pytest.Pytester) -> None:
    write_suite(pytester, SNAPSHOT_PRE_MERGE)
    result = pytester.runpytest(*NO_XDIST, *cov_args("out/a.info"))
    result.assert_outcomes(passed=30)
    written = pytester.path / "out" / "a.info"
    result.stdout.fnmatch_lines([f"popcorn: firmware coverage written to {written}"])
    assert written.read_bytes() == (pytester.path / "pre.info").read_bytes()


def test_cov_with_a_value_is_a_usage_error(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("-p", "no:asyncio", "--popcorn-cov=a.info")
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*ignored explicit argument*"])
    assert not (pytester.path / "a.info").exists()


def test_bare_flag_writes_the_ini_lcov(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.makeini("[pytest]\npopcorn_cov_lcov = out/ini.info\n")
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov")
    result.assert_outcomes(passed=30)
    assert "SF:" in (pytester.path / "out" / "ini.info").read_text()


def test_ini_reports_without_the_flag_count_nothing(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    pytester.makeini(
        "[pytest]\npopcorn_cov_lcov = ini.info\npopcorn_cov_report = lcov:other.info\n"
        "popcorn_cov_fail_under = 90\npopcorn_cov_include = **/x.c\n"
    )
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    result = pytester.runpytest_subprocess(*NO_XDIST, "-k", "test_add")
    assert result.ret == 0
    assert list(scratch.glob("popcorn-cov-*")) == []
    assert not (pytester.path / "ini.info").exists()
    assert not (pytester.path / "other.info").exists()


@pytest.mark.parametrize(
    "option",
    [
        "--popcorn-cov-report=lcov:x",
        "--popcorn-cov-fail-under=50",
        "--popcorn-cov-fail-under=0",
        "--popcorn-cov-include=**/*.c",
        "--popcorn-cov-omit=**/*.c",
    ],
)
def test_dependent_options_need_the_flag(pytester: pytest.Pytester, option: str) -> None:
    write_conftest(pytester)
    result = pytester.runpytest("-p", "no:asyncio", option)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([f"*{option.partition('=')[0]} needs --popcorn-cov*"])


@pytest.mark.parametrize(
    ("args", "ini", "message"),
    [
        (("--popcorn-cov-report=json",), "", "*unknown report kind 'json'*"),
        (("--popcorn-cov-report=lcov",), "", "*lcov needs a path*"),
        (("--popcorn-cov-report=xml:",), "", "*empty target*"),
        (("--popcorn-cov-fail-under=101",), "", "*from 0 to 100*"),
        (("--popcorn-cov-fail-under=-1",), "", "*from 0 to 100*"),
        ((), "popcorn_cov_fail_under = many", "*popcorn_cov_fail_under must be a number*"),
        ((), "popcorn_cov_report = html:", "*empty target*"),
    ],
)
def test_invalid_values_are_usage_errors(
    pytester: pytest.Pytester, args: tuple[str, ...], ini: str, message: str
) -> None:
    write_conftest(pytester)
    pytester.makeini(f"[pytest]\n{ini}\n")
    result = pytester.runpytest("-p", "no:asyncio", "--popcorn-cov", *args)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines([message])


def test_command_line_replaces_the_ini_lists(pytester: pytest.Pytester) -> None:
    pytester.makeini(
        "[pytest]\npopcorn_cov_lcov = ini.info\npopcorn_cov_report =\n    lcov:ini2.info\n"
        "popcorn_cov_include =\n    **/ini.c\npopcorn_cov_omit =\n    **/ini_omit.c\n"
        "popcorn_cov_fail_under = 10\n"
    )
    root = pytester.path
    result = check_settings(
        pytester,
        f"""
        root = Path({str(root)!r})
        assert [spec.target for spec in settings.cov_reports] == [
            root / "ini.info", root / "cli.info"
        ]
        assert settings.cov_include == ("**/cli.c",)
        assert settings.cov_omit == ("**/cli_omit.c",)
        assert settings.cov_fail_under == 75.0
        """,
        "--popcorn-cov",
        "--popcorn-cov-report=lcov:cli.info",
        "--popcorn-cov-include=**/cli.c",
        "--popcorn-cov-omit=**/cli_omit.c",
        "--popcorn-cov-fail-under=75",
    )
    result.assert_outcomes(passed=1)


def test_ini_values_apply_without_command_line_values(pytester: pytest.Pytester) -> None:
    pytester.makeini(
        "[pytest]\npopcorn_cov_report =\n    lcov:ini2.info\n    html\n"
        "popcorn_cov_include =\n    **/ini.c\npopcorn_cov_omit =\n    **/ini_omit.c\n"
        "popcorn_cov_fail_under = 12.5\n"
    )
    root = pytester.path
    result = check_settings(
        pytester,
        f"""
        root = Path({str(root)!r})
        assert [(spec.kind, spec.target) for spec in settings.cov_reports] == [
            ("lcov", root / "ini2.info"), ("html", root / "htmlcov")
        ]
        assert settings.cov_include == ("**/ini.c",)
        assert settings.cov_omit == ("**/ini_omit.c",)
        assert settings.cov_fail_under == 12.5
        """,
        "--popcorn-cov",
    )
    result.assert_outcomes(passed=1)


def test_an_ini_report_line_keeps_the_spaces_of_its_target(pytester: pytest.Pytester) -> None:
    pytester.makeini("[pytest]\npopcorn_cov_report =\n    html:out dir\n    xml:a b.xml\n")
    root = pytester.path
    result = check_settings(
        pytester,
        f"""
        root = Path({str(root)!r})
        assert [(spec.kind, spec.target) for spec in settings.cov_reports] == [
            ("html", root / "out dir"), ("xml", root / "a b.xml")
        ]
        """,
        "--popcorn-cov",
    )
    result.assert_outcomes(passed=1)


def test_html_report_without_genhtml_is_written_by_the_builtin_renderer(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    monkeypatch.setenv("PATH", str(pytester.mkdir("nobin")))
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=html:out", "-k", "test_add"
    )
    assert result.ret == pytest.ExitCode.OK
    result.stdout.fnmatch_lines([f"popcorn: firmware coverage written to {pytester.path / 'out'}"])
    assert (pytester.path / "out" / "index.html").is_file()
    assert (pytester.path / "out" / "src-0.html").is_file()


def test_a_report_that_fails_to_write_keeps_the_directory(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    (pytester.path / "blocker").write_text("a file")
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=xml:blocker/c.xml", "-k", "test_add"
    )
    result.stdout.fnmatch_lines(["popcorn: coverage merge failed:*"])
    assert not (pytester.path / "blocker" / "c.xml").exists()
    assert len(list(scratch.glob("popcorn-cov-*"))) == 1


def test_a_bare_lcov_report_takes_the_ini_lcov_once(pytester: pytest.Pytester) -> None:
    pytester.makeini("[pytest]\npopcorn_cov_lcov = ini.info\n")
    root = pytester.path
    result = check_settings(
        pytester,
        f"""
        assert [spec.target for spec in settings.cov_reports] == [
            Path({str(root / "ini.info")!r})
        ]
        """,
        "--popcorn-cov",
        "--popcorn-cov-report=lcov",
    )
    result.assert_outcomes(passed=1)


def test_targets_resolve_against_their_own_base(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    pytester.makeini("[pytest]\npopcorn_cov_lcov = ini.info\n")
    sub = pytester.mkdir("sub")
    root = pytester.path
    write_conftest(pytester)
    (sub / "test_targets.py").write_text(settings_test(f"""
        assert settings.cov_lcov == Path({str(root / "ini.info")!r})
        assert [spec.target for spec in settings.cov_reports] == [
            Path({str(root / "ini.info")!r}), Path({str(sub / "cli.info")!r})
        ]
        """))
    monkeypatch.chdir(sub)
    result = pytester.runpytest(
        "-p", "no:asyncio", "--popcorn-cov", "--popcorn-cov-report=lcov:cli.info"
    )
    result.assert_outcomes(passed=1)


def sources(lcov: Path) -> set[str]:
    return {line.split(":", 1)[1] for line in lcov.read_text().splitlines() if line[:3] == "SF:"}


def test_include_and_omit_filter_the_written_lcov(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest(*NO_XDIST, *cov_args("all.info"))
    result.assert_outcomes(passed=30)
    every = sources(pytester.path / "all.info")
    driver = {path for path in every if path.endswith("driver.c")}
    assert driver
    assert every - driver

    pytester.runpytest(*NO_XDIST, *cov_args("only.info"), "--popcorn-cov-include=**/driver.c")
    assert sources(pytester.path / "only.info") == driver

    pytester.runpytest(*NO_XDIST, *cov_args("rest.info"), "--popcorn-cov-omit=**/driver.c")
    assert sources(pytester.path / "rest.info") == every - driver


def test_ini_report_targets_resolve_against_the_rootdir_from_a_subdirectory(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    pytester.makeini("[pytest]\npopcorn_cov_report = lcov:rep.info\n")
    sub = pytester.mkdir("sub")
    write_conftest(pytester)
    (sub / "test_targets.py").write_text(settings_test(f"""
        assert [spec.target for spec in settings.cov_reports] == [
            Path({str(pytester.path / "rep.info")!r})
        ]
        """))
    monkeypatch.chdir(sub)
    pytester.runpytest("-p", "no:asyncio", "--popcorn-cov").assert_outcomes(passed=1)


TERM_HEADER = "Name*Lines*Hit*Cover*Funcs*Cover*Branches*Cover"


def count_lines(output: str, needle: str) -> int:
    return sum(1 for line in output.splitlines() if needle in line)


@pytest.mark.parametrize("quiet", [(), ("-q",)], ids=["verbose", "quiet"])
def test_term_report_prints_the_table(pytester: pytest.Pytester, quiet: tuple[str, ...]) -> None:
    write_suite(pytester)
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=term", *quiet)
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines(["*firmware coverage*", TERM_HEADER, "*driver.c*", "TOTAL*"])
    assert count_lines(result.stdout.str(), "TOTAL") == 1


def test_bare_flag_prints_the_term_table(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    result = pytester.runpytest_subprocess(*NO_XDIST, "--popcorn-cov")
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines([TERM_HEADER, "*driver.c*", "TOTAL*"])
    assert list(scratch.glob("popcorn-cov-*")) == []


def test_ini_lcov_alone_prints_no_table(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.makeini("[pytest]\npopcorn_cov_lcov = out.info\n")
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov")
    result.assert_outcomes(passed=30)
    assert "TOTAL" not in result.stdout.str()


def test_term_report_prints_once_under_xdist(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_suite(pytester)
    result = pytester.runpytest_subprocess(
        "-p", "no:asyncio", "-n", "2", "--popcorn-cov", "--popcorn-cov-report=term", timeout=60
    )
    result.assert_outcomes(passed=30)
    output = result.stdout.str()
    assert count_lines(output, "TOTAL") == 1
    assert count_lines(output, "firmware coverage ---") == 1


def test_term_row_name_is_the_display_name_of_the_source(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest(
        *NO_XDIST,
        *cov_args("cov.info"),
        "--popcorn-cov-report=term",
        "--popcorn-cov-include=**/driver.c",
    )
    result.assert_outcomes(passed=30)
    (sf,) = sources(pytester.path / "cov.info")
    expected = display_name(sf, pytester.path)
    rows = [line for line in result.stdout.str().splitlines() if "driver.c" in line]
    assert len(rows) == 1
    assert rows[0].split()[0] == expected


FAIL_UNDER = "Required coverage of {} not reached"


def line_percent(lcov: Path) -> float:
    return parse_lcov(lcov.read_text()).totals().lines.percent


@pytest.fixture
def measured(pytester: pytest.Pytester) -> tuple[float, float]:
    """The line percent of the suite over every file, and without `driver.c`."""
    write_suite(pytester)
    pytester.runpytest(*NO_XDIST, *cov_args("all.info")).assert_outcomes(passed=30)
    pytester.runpytest(*NO_XDIST, *cov_args("rest.info"), "--popcorn-cov-omit=**/driver.c")
    return line_percent(pytester.path / "all.info"), line_percent(pytester.path / "rest.info")


def test_a_threshold_above_the_total_fails_the_session(
    pytester: pytest.Pytester, measured: tuple[float, float]
) -> None:
    total, _ = measured
    assert 1 < total < 99
    threshold = int(total) + 1
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", f"--popcorn-cov-fail-under={threshold}")
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines(
        [f"FAIL {FAIL_UNDER.format(f'{threshold}%')}. Total coverage: {int(total)}.*%"]
    )


def test_a_threshold_at_or_below_the_total_passes(
    pytester: pytest.Pytester, measured: tuple[float, float]
) -> None:
    total, _ = measured
    result = pytester.runpytest(
        *NO_XDIST, "--popcorn-cov", f"--popcorn-cov-fail-under={int(total)}"
    )
    assert result.ret == pytest.ExitCode.OK
    assert "FAIL Required" not in result.stdout.str()


def test_the_threshold_boundary_is_the_cut_total(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.runpytest(*NO_XDIST, *cov_args("x.info"))
    lines = parse_lcov((pytester.path / "x.info").read_text()).totals().lines
    cut = truncated_percent(lines)
    above = Decimal(cut) + Decimal("0.01")
    passing = pytester.runpytest(*NO_XDIST, "--popcorn-cov", f"--popcorn-cov-fail-under={cut}")
    assert passing.ret == pytest.ExitCode.OK
    failing = pytester.runpytest(*NO_XDIST, "--popcorn-cov", f"--popcorn-cov-fail-under={above}")
    assert failing.ret == pytest.ExitCode.TESTS_FAILED
    failing.stdout.fnmatch_lines([f"*Total coverage: {cut}%"])


def test_omit_moves_the_total_across_the_threshold(
    pytester: pytest.Pytester, measured: tuple[float, float]
) -> None:
    total, rest = measured
    assert abs(total - rest) > 1
    low, high = sorted((total, rest))
    threshold = f"--popcorn-cov-fail-under={(low + high) / 2}"
    assert pytester.runpytest(*NO_XDIST, "--popcorn-cov", threshold).ret == (
        pytest.ExitCode.OK if total > rest else pytest.ExitCode.TESTS_FAILED
    )
    omitted = pytester.runpytest(
        *NO_XDIST, "--popcorn-cov", threshold, "--popcorn-cov-omit=**/driver.c"
    )
    assert omitted.ret == (pytest.ExitCode.TESTS_FAILED if total > rest else pytest.ExitCode.OK)


def test_a_failing_test_keeps_its_exit_status(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    (pytester.path / "test_bad.py").write_text("def test_bad():\n    assert False\n")
    result = pytester.runpytest(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-fail-under=100", "test_suite.py", "test_bad.py"
    )
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines([f"FAIL {FAIL_UNDER.format('100%')}*"])
    assert result.parseoutcomes()["failed"] == 1


def test_a_run_without_coverage_fails_under_any_threshold(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    (pytester.path / "test_plain.py").write_text("def test_plain():\n    pass\n")
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", "--popcorn-cov-fail-under=0.5")
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines(
        [
            "popcorn: no firmware coverage collected*",
            f"FAIL {FAIL_UNDER.format('0.5%')}. Total coverage: 0.00%",
        ]
    )


def test_a_run_without_coverage_passes_a_zero_threshold(
    pytester: pytest.Pytester,
) -> None:
    write_conftest(pytester)
    (pytester.path / "test_plain.py").write_text("def test_plain():\n    pass\n")
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", "--popcorn-cov-fail-under=0")
    assert result.ret == pytest.ExitCode.OK


def test_a_failed_merge_fails_under_any_threshold(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    monkeypatch.setenv("TMPDIR", str(pytester.mkdir("scratch")))
    (pytester.path / "blocker").write_text("a file")
    result = pytester.runpytest_subprocess(
        *NO_XDIST,
        "--popcorn-cov",
        "--popcorn-cov-report=xml:blocker/c.xml",
        "--popcorn-cov-fail-under=1",
        "-k",
        "test_add",
    )
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines(
        [
            "popcorn: coverage merge failed*",
            f"FAIL {FAIL_UNDER.format('1%')}. Total coverage: 0.00%",
        ]
    )


def test_an_interrupted_session_skips_the_check(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    (pytester.path / "test_stop.py").write_text("def test_stop():\n    raise KeyboardInterrupt\n")
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-fail-under=100", "test_suite.py", "test_stop.py"
    )
    assert result.ret == pytest.ExitCode.INTERRUPTED
    assert "FAIL Required" not in result.stdout.str()
    assert len(list(scratch.glob("popcorn-cov-*"))) == 1


@pytest.mark.parametrize(
    ("threshold", "shown"),
    [("12.3456789", "12.3456789"), ("0.00001", "0.00001"), ("75.0", "75"), ("89.996", "89.996")],
)
def test_the_fail_line_prints_the_threshold_as_given(
    pytester: pytest.Pytester, threshold: str, shown: str
) -> None:
    write_conftest(pytester)
    (pytester.path / "test_plain.py").write_text("def test_plain():\n    pass\n")
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", f"--popcorn-cov-fail-under={threshold}")
    result.stdout.fnmatch_lines([f"FAIL {FAIL_UNDER.format(f'{shown}%')}. Total coverage: 0.00%"])


def test_a_status_other_than_ok_is_kept(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-fail-under=50", "-k", "nothing_matches_this"
    )
    assert result.ret == pytest.ExitCode.NO_TESTS_COLLECTED
    result.stdout.fnmatch_lines([f"FAIL {FAIL_UNDER.format('50%')}*"])


def cobertura(path: Path) -> ElementTree.Element:
    return ElementTree.parse(path).getroot()


def test_xml_report_matches_the_term_totals(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest(
        *NO_XDIST,
        "--popcorn-cov",
        "--popcorn-cov-report=xml:out/c.xml",
        "--popcorn-cov-report=term",
    )
    result.assert_outcomes(passed=30)
    out = pytester.path / "out" / "c.xml"
    result.stdout.fnmatch_lines([f"popcorn: firmware coverage written to {out}"])
    top = cobertura(out)
    (total,) = [line.split() for line in result.stdout.str().splitlines() if "TOTAL" in line]
    assert top.get("lines-valid") == total[1]
    assert top.get("lines-covered") == total[2]
    assert top.get("branches-valid") == total[6]
    paths = [
        element.get("filename", "") for element in top.iterfind("./packages/package/classes/class")
    ]
    assert len(paths) == 2
    assert not any(Path(path).is_absolute() for path in paths)
    assert [element.text for element in top.iterfind("./sources/source")] == [str(pytester.path)]


def test_xml_report_of_a_relative_sf_keeps_the_sf_as_filename(pytester: pytest.Pytester) -> None:
    lcov = session(
        pytester,
        CUT_ELF,
        "cut_mid",
        CUT_TESTS,
        "plain",
        "-p",
        "no:xdist",
        "--popcorn-cov-report=xml:c.xml",
    )
    sf = [line[3:] for line in lcov.read_text().splitlines() if line[:3] == "SF:"]
    assert sf
    assert not Path(sf[0]).is_absolute()
    top = cobertura(pytester.path / "c.xml")
    filenames = [
        element.get("filename") for element in top.iterfind("./packages/package/classes/class")
    ]
    assert filenames == sf
    totals = parse_lcov(lcov.read_text()).totals()
    assert float(top.get("line-rate", "")) == totals.lines.hit / totals.lines.found


def test_xml_report_without_a_target_writes_coverage_xml(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=xml")
    result.assert_outcomes(passed=30)
    assert cobertura(pytester.path / "coverage.xml").tag == "coverage"


needs_genhtml = pytest.mark.skipif(
    shutil.which("genhtml") is None, reason="genhtml is not installed"
)


@needs_genhtml
def test_html_report_of_driver_firmware_renders_without_errors(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=html:out", "-k", "test_add"
    )
    assert result.ret == pytest.ExitCode.OK
    out = pytester.path / "out"
    result.stdout.fnmatch_lines([f"popcorn: firmware coverage written to {out}"])
    assert (out / "index.html").is_file()
    assert "ERROR" not in result.stdout.str() + result.stderr.str()
    assert list(scratch.glob("popcorn-cov-*")) == []


@needs_genhtml
def test_html_report_finds_relative_sources_from_the_rootdir_not_the_cwd(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    project = pytester.mkdir("project")
    source = project / "tests" / "firmware" / "obs"
    source.mkdir(parents=True)
    shutil.copy(CUT_SOURCE, source / "cut.c")
    write_conftest(pytester)
    (pytester.path / "conftest.py").replace(project / "conftest.py")
    write_project(pytester, "cut_mid", elf=CUT_ELF, directory=project)
    (project / "test_cov.py").write_text(CUT_TESTS)
    elsewhere = pytester.mkdir("elsewhere")
    monkeypatch.chdir(elsewhere)
    result = pytester.runpytest_subprocess(
        "-p",
        "no:asyncio",
        "-p",
        "no:xdist",
        f"--rootdir={project}",
        "--popcorn-cov",
        f"--popcorn-cov-report=html:{elsewhere / 'out'}",
        str(project),
    )
    assert result.ret == pytest.ExitCode.OK
    pages = "".join(page.read_text() for page in (elsewhere / "out").rglob("cut.c.gcov.html"))
    assert "use_pick" in pages
    assert "pick(v) * 2u" in pages


@needs_genhtml
def test_html_report_leaves_a_foreign_directory_and_keeps_the_exit_status(
    pytester: pytest.Pytester,
) -> None:
    write_suite(pytester)
    foreign = pytester.mkdir("foreign")
    (foreign / "notes.txt").write_text("mine")
    result = pytester.runpytest_subprocess(
        *NO_XDIST,
        "--popcorn-cov",
        f"--popcorn-cov-report=html:{foreign}",
        "--popcorn-cov-report=term",
        "-k",
        "test_add",
    )
    assert result.ret == pytest.ExitCode.OK
    result.stdout.fnmatch_lines(
        [
            f"popcorn: html report not written, {foreign} is not empty "
            "and holds no earlier html report"
        ]
    )
    assert [path.name for path in foreign.iterdir()] == ["notes.txt"]
    assert "TOTAL" in result.stdout.str()


@needs_genhtml
def test_a_refused_html_target_keeps_the_pcov_files(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    foreign = pytester.mkdir("src")
    (foreign / "notes.txt").write_text("mine")
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", f"--popcorn-cov-report=html:{foreign}", "-k", "test_add"
    )
    result.stdout.fnmatch_lines(
        [
            "popcorn: html report not written, *",
            "popcorn: a report was not written, the .pcov files stay in *",
        ]
    )
    kept = list(scratch.glob("popcorn-cov-*"))
    assert len(kept) == 1
    assert list(kept[0].glob("*.pcov"))
    assert [path.name for path in foreign.iterdir()] == ["notes.txt"]


@needs_genhtml
def test_html_report_at_the_rootdir_leaves_the_project_alone(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    (pytester.path / "index.html").write_text("mine")
    (pytester.path / "gcov.css").write_text("mine")
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=html:.", "-k", "test_add"
    )
    assert result.ret == pytest.ExitCode.OK
    result.stdout.fnmatch_lines(["popcorn: html report not written, * holds the rootdir"])
    assert (pytester.path / "index.html").read_text() == "mine"
    assert (pytester.path / "gcov.css").read_text() == "mine"


@needs_genhtml
def test_html_report_replaces_an_earlier_report(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    out = pytester.mkdir("out")
    (out / "index.html").write_text("old")
    (out / "gcov.css").write_text("old")
    (out / "stale.txt").write_text("old")
    result = pytester.runpytest_subprocess(
        *NO_XDIST, "--popcorn-cov", "--popcorn-cov-report=html:out", "-k", "test_add"
    )
    assert result.ret == pytest.ExitCode.OK
    assert not (out / "stale.txt").exists()
    assert (out / "index.html").read_text() != "old"


@needs_genhtml
def test_html_report_renders_the_documented_genhtml_message(pytester: pytest.Pytester) -> None:
    out = pytester.path / "out"
    session(
        pytester,
        SHARED_EXIT_ELF,
        "settled",
        SHARED_EXIT_TESTS,
        "shared",
        "-p",
        "no:xdist",
        f"--popcorn-cov-report=html:{out}",
    )
    assert (out / "index.html").is_file()


GUIDE_PYPROJECT = ROOT / "tests" / "docs" / "guide" / "coverage_pyproject.toml"


def test_the_documented_pyproject_block_configures_the_reports(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    (pytester.path / "pyproject.toml").write_text(GUIDE_PYPROJECT.read_text())
    result = pytester.runpytest(*NO_XDIST, "--popcorn-cov")
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines(
        ["FAIL Required coverage of 80% not reached.*", "*firmware coverage*", "TOTAL*"]
    )
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    out = pytester.path / "build" / "cov"
    assert (out / "html" / "index.html").is_file()
    assert cobertura(out / "coverage.xml").tag == "coverage"
    assert sources(out / "firmware.info") == {
        "tests/firmware/api/callee.c",
        "tests/firmware/api/driver.c",
    }

"""Coverage options: the flag, the report specs, the ini options and their precedence."""

from __future__ import annotations

import textwrap
from pathlib import Path

import pytest
from popcorn.coverage.report import display_name

from .coverage_suite import write_suite
from .helpers import cov_args, write_conftest

pytest_plugins = ["pytester"]

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


def test_an_unavailable_report_writes_no_file_and_keeps_the_directory(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_suite(pytester)
    scratch = pytester.mkdir("scratch")
    monkeypatch.setenv("TMPDIR", str(scratch))
    result = pytester.runpytest_subprocess(
        *NO_XDIST, *cov_args("a.info"), "--popcorn-cov-report=xml", "-k", "test_add"
    )
    result.stdout.fnmatch_lines(
        ["popcorn: coverage merge failed: the xml report is not available*"]
    )
    assert not (pytester.path / "a.info").exists()
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
